/**
 * Copyright 2020 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/runtime/kernel/arm/fp32/convolution_winograd.h"
#include "nnacl/fp32/conv.h"
#include "schema/model_generated.h"
#include "src/kernel_registry.h"
#include "include/errorcode.h"
#include "src/runtime/runtime_api.h"

using mindspore::kernel::KERNEL_ARCH::kCPU;
using mindspore::lite::KernelRegistrar;
using mindspore::lite::RET_ERROR;
using mindspore::lite::RET_OK;
using mindspore::schema::PrimitiveType_Conv2D;

namespace mindspore::kernel {
int ConvolutionWinogradCPUKernel::WinogradFilterTransform(const float *weight_data, float *matrix_g, float *matrix_gt,
                                                          int oc_block) {
  // original weight format : ohwi
  auto channel_in = conv_param_->input_channel_;
  auto channel_out = conv_param_->output_channel_;
  int input_unit_square = input_unit_ * input_unit_;
  int ic4 = UP_DIV(channel_in, C4NUM);
  int oc_block_num = UP_DIV(channel_out, oc_block);

  // trans_filter = G*g*GT (g represents weight_data)
  // separate into two steps ===> tmp = G*g ===> out = tmp * GT
  auto tmp_weight_data = reinterpret_cast<float *>(malloc(kernel_unit_ * kernel_unit_ * sizeof(float)));
  auto tmp_data = reinterpret_cast<float *>(malloc(input_unit_ * kernel_unit_ * sizeof(float)));
  auto trans_out_data = reinterpret_cast<float *>(malloc(input_unit_ * input_unit_ * sizeof(float)));
  std::vector<int> shape{input_unit_ * input_unit_, oc_block_num, ic4, C4NUM, oc_block};
  std::vector<int> strides;
  for (int i = 0; i < 4; i++) {
    int stride = 1;
    for (int j = i + 1; j < 5; j++) {
      stride *= shape[j];
    }
    strides.push_back(stride);
  }

  int kernel_plane_stride = channel_in;
  if (oc_block == 0) {
    MS_LOG(ERROR) << "Divide by zero";
    free(tmp_weight_data);
    free(tmp_data);
    free(trans_out_data);
    return RET_ERROR;
  }
  for (int i = 0; i < channel_out; i++) {
    int out_c_block = i / oc_block;
    int out_c_res = i % oc_block;
    int input_oz_offset = i * kernel_unit_ * kernel_unit_ * channel_in;
    int output_oz_offset = out_c_block * strides[1] + out_c_res;
    for (int j = 0; j < channel_in; j++) {
      int ic4_block = j / C4NUM;
      int ic4_res = j % C4NUM;
      int input_iz_offset = input_oz_offset + j;
      int output_iz_offset = output_oz_offset + ic4_block * strides[2] + ic4_res * strides[3];
      for (int k = 0; k < kernel_unit_ * kernel_unit_; k++) {
        int input_xy_offset = input_iz_offset + k * kernel_plane_stride;
        tmp_weight_data[k] = *(weight_data + input_xy_offset);
      }
      // now we only support row-major matrix-multiply
      // tmp = G * g
      MatrixMultiply(matrix_g, tmp_weight_data, tmp_data, input_unit_, kernel_unit_, kernel_unit_);
      // out = tmp * GT
      MatrixMultiply(tmp_data, matrix_gt, trans_out_data, input_unit_, kernel_unit_, input_unit_);

      for (int z = 0; z < input_unit_square; z++) {
        int output_xy_offset = output_iz_offset + z * strides[0];
        *(trans_weight_ + output_xy_offset) = trans_out_data[z];
      }
    }
  }
  free(tmp_weight_data);
  free(tmp_data);
  free(trans_out_data);
  return RET_OK;
}

int ConvolutionWinogradCPUKernel::InitWeightBias() {
  auto filter_tensor = in_tensors_.at(kWeightIndex);
  int in_channel = filter_tensor->Channel();
  int out_channel = filter_tensor->Batch();
  int ic4 = UP_DIV(in_channel, C4NUM);
  conv_param_->input_channel_ = in_channel;
  conv_param_->output_channel_ = out_channel;

  int oc4 = UP_DIV(out_channel, C4NUM);
  int oc_block, oc_block_num;
  oc_block = C8NUM;
  oc_block_num = UP_DIV(out_channel, C8NUM);

  // set data
  auto trans_matrix_data_size = input_unit_ * input_unit_ * ic4 * C4NUM * oc_block_num * oc_block * sizeof(float);
  trans_weight_ = reinterpret_cast<float *>(malloc(trans_matrix_data_size));
  if (trans_weight_ == nullptr) {
    MS_LOG(ERROR) << "malloc matrix_buffer failed.";
    return RET_ERROR;
  }

  memset(trans_weight_, 0, trans_matrix_data_size);

  float matrix_g[64];
  float matrix_gt[64];
  float matrix_a[64];
  float matrix_at[64];
  float matrix_b[64];
  float matrix_bt[64];
  CookToomFilter(matrix_a, matrix_at, matrix_b, matrix_bt, matrix_g, matrix_gt, 1.0f, output_unit_, kernel_unit_);

  auto weight_data = reinterpret_cast<float *>(filter_tensor->MutableData());
  auto ret = WinogradFilterTransform(weight_data, matrix_g, matrix_gt, oc_block);
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "winograd filter transfrom failed.";
    return ret;
  }

  // init bias
  size_t new_bias_size = oc4 * C4NUM * sizeof(float);
  bias_data_ = reinterpret_cast<float *>(malloc(new_bias_size));
  memset(bias_data_, 0, new_bias_size);
  if (in_tensors_.size() == kInputSize2) {
    auto ori_bias_addr = reinterpret_cast<float *>(in_tensors_.at(kBiasIndex)->MutableData());
    memcpy(bias_data_, ori_bias_addr, out_channel * sizeof(float));
  } else {
    MS_ASSERT(in_tensors_.size() == kInputSize1);
  }
  return RET_OK;
}

int ConvolutionWinogradCPUKernel::InitTmpBuffer() {
  int channel_out = conv_param_->output_channel_;
  int output_h = conv_param_->output_h_;
  int output_w = conv_param_->output_w_;
  int oc4 = UP_DIV(channel_out, C4NUM);
  int oc8 = UP_DIV(channel_out, C8NUM);
  int ic4 = UP_DIV(conv_param_->input_channel_, C4NUM);
#ifdef ENABLE_ARM32
  int tile_num = 4;
#else
  int tile_num = 12;
#endif
  MS_ASSERT(ctx_->allocator != nullptr);

  size_t nhwc4_input_size =
    ic4 * C4NUM * conv_param_->input_batch_ * conv_param_->input_h_ * conv_param_->input_w_ * sizeof(float);
  nhwc4_input_ = ctx_->allocator->Malloc(nhwc4_input_size);
  if (nhwc4_input_ == nullptr) {
    MS_LOG(ERROR) << "malloc nhwc4_input_ failed.";
    return RET_ERROR;
  }

  size_t tile_buffer_size = thread_count_ * tile_num * input_unit_ * input_unit_ * ic4 * C4NUM * sizeof(float);
  trans_input_ = reinterpret_cast<float *>(ctx_->allocator->Malloc(tile_buffer_size));
  if (trans_input_ == nullptr) {
    MS_LOG(ERROR) << "malloc trans_input_ failed.";
    return RET_ERROR;
  }

  gemm_out_ = reinterpret_cast<float *>(
    ctx_->allocator->Malloc(thread_count_ * tile_num * input_unit_ * input_unit_ * oc8 * C8NUM * sizeof(float)));
  if (gemm_out_ == nullptr) {
    MS_LOG(ERROR) << "malloc gemm_out_ failed.";
    return RET_ERROR;
  }

  int out_w_block = UP_DIV(output_w, output_unit_);
  int out_h_block = UP_DIV(output_h, output_unit_);
  tmp_out_data_ =
    reinterpret_cast<float *>(ctx_->allocator->Malloc(conv_param_->output_batch_ * out_w_block * out_h_block *
                                                      output_unit_ * output_unit_ * oc4 * C4NUM * sizeof(float)));
  if (tmp_out_data_ == nullptr) {
    MS_LOG(ERROR) << "malloc tmp_out_data_ failed.";
    return RET_ERROR;
  }

  tmp_data_ = reinterpret_cast<float *>(
    ctx_->allocator->Malloc(thread_count_ * C4NUM * input_unit_ * input_unit_ * sizeof(float)));
  if (tmp_data_ == nullptr) {
    MS_LOG(ERROR) << "malloc tmp_data_ failed.";
    return RET_ERROR;
  }

  col_buffer_ =
    reinterpret_cast<float *>(ctx_->allocator->Malloc(thread_count_ * tile_num * ic4 * C4NUM * sizeof(float)));
  if (col_buffer_ == nullptr) {
    MS_LOG(ERROR) << "malloc col_buffer_ failed.";
    return RET_ERROR;
  }

  tmp_buffer_address_list_[0] = trans_input_;
  tmp_buffer_address_list_[1] = gemm_out_;
  tmp_buffer_address_list_[2] = tmp_out_data_;
  tmp_buffer_address_list_[3] = tmp_data_;
  tmp_buffer_address_list_[4] = col_buffer_;
  return RET_OK;
}

int ConvolutionWinogradCPUKernel::ConfigInputOutput() {
  auto output_tensor = out_tensors_.at(kOutputIndex);
  output_tensor->SetFormat(schema::Format::Format_NHWC);

  in_func_ = GetInputTransFunc(input_unit_);
  if (in_func_ == nullptr) {
    MS_LOG(ERROR) << "in_func_ is null.";
    return RET_ERROR;
  }
  out_func_ = GetOutputTransFunc(input_unit_, output_unit_);
  if (out_func_ == nullptr) {
    MS_LOG(ERROR) << "out_func_ is null.";
    return RET_ERROR;
  }

  // #ifdef ENABLE_ARM32
  //   gemm_func_ = IndirectGemmFp32_8x4;
  // #else
  gemm_func_ = IndirectGemmFp32_8x8;
  // #endif
  return RET_OK;
}

int ConvolutionWinogradCPUKernel::Init() {
  kernel_unit_ = conv_param_->kernel_h_;
  input_unit_ = output_unit_ + kernel_unit_ - 1;
  conv_param_->input_unit_ = input_unit_;
  conv_param_->output_unit_ = output_unit_;
  auto ret = InitWeightBias();
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "Init weight bias failed.";
    return RET_ERROR;
  }
  if (!InferShapeDone()) {
    return RET_OK;
  }
  return ReSize();
}

int ConvolutionWinogradCPUKernel::ReSize() {
  auto ret = ConvolutionBaseCPUKernel::CheckResizeValid();
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "Resize is invalid.";
    return ret;
  }

  ret = ConvolutionBaseCPUKernel::Init();
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "ConvolutionBase init failed.";
    return RET_ERROR;
  }

  kernel_unit_ = conv_param_->kernel_h_;
  input_unit_ = output_unit_ + kernel_unit_ - 1;
  conv_param_->input_unit_ = input_unit_;
  conv_param_->output_unit_ = output_unit_;

  ret = ConfigInputOutput();
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "ConfigInputOutput failed.";
    return RET_ERROR;
  }
  return RET_OK;
}

int ConvolutionWinogradCPUKernel::RunImpl(int task_id) {
  if (gemm_func_ == nullptr) {
    MS_LOG(ERROR) << "gemm_func is nullptr.";
    return RET_ERROR;
  }
  ConvWinogardFp32(reinterpret_cast<float *>(nhwc4_input_), trans_weight_, reinterpret_cast<const float *>(bias_data_),
                   tmp_buffer_address_list_, task_id, conv_param_, in_func_, out_func_, gemm_func_);
  return RET_OK;
}

int ConvolutionWinogradImpl(void *cdata, int task_id) {
  auto conv = reinterpret_cast<ConvolutionWinogradCPUKernel *>(cdata);
  auto error_code = conv->RunImpl(task_id);
  if (error_code != RET_OK) {
    MS_LOG(ERROR) << "ConvolutionWinograd Run error task_id[" << task_id << "] error_code[" << error_code << "]";
    return RET_ERROR;
  }
  return RET_OK;
}

int ConvolutionWinogradCPUKernel::PostProcess() {
  auto out_tensor = out_tensors_.front();
  auto out_data = reinterpret_cast<float *>(out_tensor->MutableData());
  auto act_type = conv_param_->act_type_;
  switch (act_type) {
    case ActType_No:
      UnPackWinogradOutput(tmp_out_data_, out_data, conv_param_->output_batch_, conv_param_->output_h_,
                           conv_param_->output_w_, conv_param_->output_channel_, output_unit_);
      break;
    case ActType_Relu:
      UnPackWinogradReluOutput(tmp_out_data_, out_data, conv_param_->output_batch_, conv_param_->output_h_,
                               conv_param_->output_w_, conv_param_->output_channel_, output_unit_);
      break;
    case ActType_Relu6:
      UnPackWinogradRelu6Output(tmp_out_data_, out_data, conv_param_->output_batch_, conv_param_->output_h_,
                                conv_param_->output_w_, conv_param_->output_channel_, output_unit_);
      break;
    default:
      MS_LOG(ERROR) << "Unsupport activation type.";
      return RET_ERROR;
  }
  return RET_OK;
}

int ConvolutionWinogradCPUKernel::Run() {
  auto prepare_ret = Prepare();
  if (prepare_ret != RET_OK) {
    MS_LOG(ERROR) << "Prepare fail!ret: " << prepare_ret;
    return prepare_ret;
  }

  auto ret = InitTmpBuffer();
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "Init tmp buffer failed.";
    return RET_ERROR;
  }

  auto input_tensor = in_tensors_.at(kInputIndex);
  auto ori_input_data = input_tensor->MutableData();
  PackNHWCToNHWC4Fp32(ori_input_data, nhwc4_input_, conv_param_->input_batch_,
                      conv_param_->input_h_ * conv_param_->input_w_, conv_param_->input_channel_);

  int error_code = ParallelLaunch(THREAD_POOL_DEFAULT, ConvolutionWinogradImpl, this, thread_count_);
  if (error_code != RET_OK) {
    MS_LOG(ERROR) << "conv winograd error error_code[" << error_code << "]";
    FreeTmpBuffer();
    return RET_ERROR;
  }

  ret = PostProcess();
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "Post process failed.";
    FreeTmpBuffer();
    return ret;
  }
  FreeTmpBuffer();
  return RET_OK;
}
}  // namespace mindspore::kernel
