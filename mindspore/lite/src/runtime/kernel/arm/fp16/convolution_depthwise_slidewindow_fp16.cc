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

#include "src/runtime/kernel/arm/fp16/convolution_depthwise_slidewindow_fp16.h"
#include "nnacl/fp16/pack_fp16.h"
#include "nnacl/fp16/cast_fp16.h"
#include "include/errorcode.h"

using mindspore::lite::RET_ERROR;
using mindspore::lite::RET_OK;

namespace mindspore::kernel {
ConvolutionDepthwiseSWFp16CPUKernel::~ConvolutionDepthwiseSWFp16CPUKernel() {
  if (sliding_ != nullptr) {
    delete sliding_;
    sliding_ = nullptr;
  }
  if (packed_weight_ != nullptr) {
    free(packed_weight_);
    packed_weight_ = nullptr;
  }
}

int ConvolutionDepthwiseSWFp16CPUKernel::InitPackedInputOutput() {
  if (conv_param_->input_channel_ % C8NUM != 0) {
    need_align_ = true;
    int C8 = UP_DIV(conv_param_->input_channel_, C8NUM);
    int pack_input_size = conv_param_->input_batch_ * conv_param_->input_h_ * conv_param_->input_w_ * C8NUM * C8;
    packed_input_ = reinterpret_cast<float16_t *>(context_->allocator->Malloc(pack_input_size * sizeof(float16_t)));
    if (packed_input_ == nullptr) {
      MS_LOG(ERROR) << "Malloc buffer failed.";
      return RET_ERROR;
    }

    int pack_output_size = conv_param_->output_batch_ * conv_param_->output_h_ * conv_param_->output_w_ * C8NUM * C8;
    packed_output_ = reinterpret_cast<float16_t *>(context_->allocator->Malloc(pack_output_size * sizeof(float16_t)));
    if (packed_output_ == nullptr) {
      MS_LOG(ERROR) << "Malloc buffer failed.";
      context_->allocator->Free(packed_input_);
      return RET_ERROR;
    }
  }
  return RET_OK;
}

int ConvolutionDepthwiseSWFp16CPUKernel::InitWeightBias() {
  // init weight: o, h, w, i; o == group, i == 1
  auto weight_tensor = in_tensors_.at(kWeightIndex);
  int OC8 = UP_DIV(weight_tensor->Batch(), C8NUM);
  int pack_weight_size = C8NUM * OC8 * weight_tensor->Height() * weight_tensor->Width();
  auto origin_weight = reinterpret_cast<float16_t *>(weight_tensor->data_c());
  MS_ASSERT(origin_weight != nullptr);

  if (packed_weight_ == nullptr) {
    packed_weight_ = reinterpret_cast<float16_t *>(malloc(pack_weight_size * sizeof(float16_t)));
    if (packed_weight_ == nullptr) {
      MS_LOG(ERROR) << "Malloc buffer failed.";
      return RET_ERROR;
    }
  }
  PackNCHWFp16ToNC8HW8Fp16(origin_weight, packed_weight_, 1, weight_tensor->Height() * weight_tensor->Width(),
                           weight_tensor->Batch());

  if (bias_data_ == nullptr) {
    bias_data_ = reinterpret_cast<float16_t *>(malloc(C8NUM * OC8 * sizeof(float16_t)));
    if (bias_data_ == nullptr) {
      MS_LOG(ERROR) << "Malloc buffer failed.";
      return RET_ERROR;
    }
  }
  memset(bias_data_, 0, C8NUM * OC8 * sizeof(float16_t));
  if (in_tensors_.size() == kInputSize2) {
    auto bias_tensor = in_tensors_.at(kBiasIndex);
    auto ori_bias = reinterpret_cast<float16_t *>(bias_tensor->data_c());
    memcpy(bias_data_, ori_bias, bias_tensor->Size());
  }

  conv_param_->thread_num_ = MSMIN(thread_count_, OC8);
  return RET_OK;
}  // namespace mindspore::kernel

int ConvolutionDepthwiseSWFp16CPUKernel::Init() {
  sliding_ = new (std::nothrow) SlidingWindowParam;
  if (sliding_ == nullptr) {
    MS_LOG(ERROR) << "new sliding window param failed.";
    return RET_ERROR;
  }

  auto ret = InitWeightBias();
  if (ret != 0) {
    MS_LOG(ERROR) << "Convolution depthwise fp16 InitWeightBias failed.";
    return RET_ERROR;
  }

  if (!InferShapeDone()) {
    return RET_OK;
  }
  return ReSize();
}

int ConvolutionDepthwiseSWFp16CPUKernel::ReSize() {
  auto ret = ConvolutionBaseCPUKernel::Init();
  if (ret != RET_OK) {
    return ret;
  }
  InitSlidingParamConvDw(sliding_, conv_param_, C8NUM);
  return RET_OK;
}

int ConvolutionDepthwiseSWFp16CPUKernel::Execute(int task_id) {
  ConvDwC8Fp16(packed_output_, packed_input_, packed_weight_, reinterpret_cast<float16_t *>(bias_data_), conv_param_,
               sliding_, task_id);
  return RET_OK;
}

static int ConvDwSWFp16Run(void *cdata, int task_id, float lhs_scale, float rhs_scale) {
  auto conv_dw_fp16 = reinterpret_cast<ConvolutionDepthwiseSWFp16CPUKernel *>(cdata);
  auto ret = conv_dw_fp16->Execute(task_id);
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "ConvolutionDepthwiseSWFp16Run error task_id[" << task_id << "] error_code[" << ret << "]";
    return RET_ERROR;
  }
  return RET_OK;
}

int ConvolutionDepthwiseSWFp16CPUKernel::Run() {
  auto ret = InitPackedInputOutput();
  if (ret != 0) {
    MS_LOG(ERROR) << "Convolution depthwise fp16 InitPackedInputOutput failed.";
    FreePackedInputOutput();
    return ret;
  }

  auto input_ptr = reinterpret_cast<float16_t *>(in_tensors_.at(0)->data_c());
  auto output_ptr = reinterpret_cast<float16_t *>(out_tensors_.at(0)->data_c());
  MS_ASSERT(input_ptr != nullptr);
  MS_ASSERT(output_ptr != nullptr);
  if (input_ptr == nullptr || output_ptr == nullptr) {
    MS_LOG(ERROR) << "Convolution depthwise Fp16 get null tensor data!";
    return RET_ERROR;
  }

  if (need_align_) {
    PackNHWCToNHWC8Fp16(input_ptr, packed_input_, conv_param_->input_batch_,
                        conv_param_->input_h_ * conv_param_->input_w_, conv_param_->input_channel_);
  } else {
    packed_input_ = input_ptr;
    packed_output_ = output_ptr;
  }

  if (is_trainable() && (IsTrain() || is_repack())) {
    ret = InitWeightBias();
    if (ret != 0) {
      MS_LOG(ERROR) << "Convolution depthwise fp16 repack weight failure";
      return RET_ERROR;
    }
    is_repack_ = false;
  }
  ret = static_cast<const lite::InnerContext *>(this->context_)
          ->thread_pool_->ParallelLaunch(ConvDwSWFp16Run, this, conv_param_->thread_num_);
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "ConvDwSWFp16Run error: error_code[" << ret << "]";
  }
  if (need_align_) {
    PackNHWC8ToNHWCFp16(packed_output_, output_ptr, conv_param_->output_batch_,
                        conv_param_->output_h_ * conv_param_->output_w_, conv_param_->output_channel_);
  }

  FreePackedInputOutput();
  return ret;
}

void ConvolutionDepthwiseSWFp16CPUKernel::FreePackedInputOutput() {
  if (need_align_) {
    context_->allocator->Free(packed_input_);
    context_->allocator->Free(packed_output_);
    packed_input_ = nullptr;
    packed_output_ = nullptr;
  }
}

int ConvolutionDepthwiseSWFp16CPUKernel::Eval() {
  if (is_trainable()) {
    is_repack_ = true;
  }
  return InnerKernel::Eval();
}
}  // namespace mindspore::kernel
