/**
 * Copyright 2019 Huawei Technologies Co., Ltd
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

#include <string>
#include <set>
#include "nnacl/fp32/common_func.h"
#include "src/kernel_registry.h"
#include "src/runtime/opencl/opencl_runtime.h"
#include "src/runtime/kernel/opencl/kernel/conv2d_transpose.h"
#ifndef PROGRAM_WITH_IL
#include "src/runtime/kernel/opencl/cl/conv2d_transpose2x2.cl.inc"
#endif

using mindspore::kernel::KERNEL_ARCH::kGPU;
using mindspore::lite::KernelRegistrar;
using mindspore::schema::PrimitiveType_DeConv2D;

namespace mindspore::kernel {

int Conv2dTransposeOpenCLKernel::Init() {
  ConvParameter *param = reinterpret_cast<ConvParameter *>(op_parameter_);
  if (param->kernel_h_ != 2 || param->kernel_w_ != 2 || param->stride_h_ != 2 || param->stride_w_ != 2) {
    MS_LOG(ERROR) << "only support kh=kw=2 and stride_h=stride_w=2.";
    return RET_ERROR;
  }
  if (param->pad_u_ != 0 || param->pad_l_ != 0) {
    MS_LOG(ERROR) << "only support pad =0.";
    return RET_ERROR;
  }
  std::string kernel_name = "conv2d_transpose2x2_" + std::string(EnumNameFormat(op_format_));
  auto ocl_runtime = lite::opencl::OpenCLRuntime::GetInstance();
  enable_fp16_ = ocl_runtime->GetFp16Enable();
#ifdef PROGRAM_WITH_IL
  kernel_ = ocl_runtime->GetKernelFromBinary(kernel_name);
#else
  std::string source = conv2d_transpose2x2_source;
  std::set<std::string> build_options;
  std::string program_name = "conv2d_transpose2x2";
  ocl_runtime->LoadSource(program_name, source);
  ocl_runtime->BuildKernel(kernel_, program_name, kernel_name, build_options);
#endif
  PadWeight();
  in_ori_format_ = in_tensors_[0]->GetFormat();
  in_tensors_[0]->SetFormat(op_format_);
  out_ori_format_ = out_tensors_[0]->GetFormat();
  out_tensors_[0]->SetFormat(op_format_);
  MS_LOG(DEBUG) << kernel_name << " Init Done!";
  return RET_OK;
}

int Conv2dTransposeOpenCLKernel::ReSize() { return RET_OK; }

void Conv2dTransposeOpenCLKernel::PadWeight() {
  ConvParameter *param = reinterpret_cast<ConvParameter *>(op_parameter_);
  int ci = in_tensors_[0]->Channel();
  int co = out_tensors_[0]->Channel();
  int kh = param->kernel_h_;
  int kw = param->kernel_w_;
  int div_ci = UP_DIV(ci, C4NUM);
  int div_co = UP_DIV(co, C4NUM);
  auto allocator = lite::opencl::OpenCLRuntime::GetInstance()->GetAllocator();
  auto data_size = enable_fp16_ ? sizeof(int16_t) : sizeof(float);

  // IHWO to OHWI4(I)4(O)(converter format is IHWO)
  // init padWeight_(buffer mem)
  padWeight_ = allocator->Malloc(div_ci * div_co * C4NUM * C4NUM * kh * kw * data_size);
  padWeight_ = allocator->MapBuffer(padWeight_, CL_MAP_WRITE, nullptr, true);
  memset(padWeight_, 0x00, div_ci * div_co * C4NUM * C4NUM * kh * kw * data_size);
  auto origin_weight = in_tensors_.at(kWeightIndex)->data_c();
  auto weight_dtype = in_tensors_.at(kWeightIndex)->data_type();
  int index = 0;
  for (int co_i = 0; co_i < div_co; co_i++) {
    for (int kh_i = 0; kh_i < kh; kh_i++) {
      for (int kw_i = 0; kw_i < kw; kw_i++) {
        for (int ci_i = 0; ci_i < div_ci; ci_i++) {
          for (int ci4_i = 0; ci4_i < C4NUM; ci4_i++) {
            for (int co4_i = 0; co4_i < C4NUM; co4_i++) {
              int co_offset = co_i * C4NUM + co4_i;
              int ci_offset = ci_i * C4NUM + ci4_i;
              if (co_offset < co && ci_offset < ci) {
                int ori_index = ((ci_offset * kh + kh_i) * kw + kw_i) * co + co_offset;
                if (enable_fp16_) {
                  if (weight_dtype == kNumberTypeFloat32) {
                    reinterpret_cast<float16_t *>(padWeight_)[index++] =
                      reinterpret_cast<float *>(origin_weight)[ori_index];
                  } else {
                    reinterpret_cast<float16_t *>(padWeight_)[index++] =
                      reinterpret_cast<float16_t *>(origin_weight)[ori_index];
                  }
                } else {
                  if (weight_dtype == kNumberTypeFloat32) {
                    reinterpret_cast<float *>(padWeight_)[index++] =
                      reinterpret_cast<float *>(origin_weight)[ori_index];
                  } else {
                    reinterpret_cast<float *>(padWeight_)[index++] =
                      reinterpret_cast<float16_t *>(origin_weight)[ori_index];
                  }
                }
              } else {
                index++;
              }
            }
          }
        }
      }
    }
  }
  allocator->UnmapBuffer(padWeight_);

  // init bias_(image2d mem)
  size_t im_dst_x, im_dst_y;
  im_dst_x = div_co;
  im_dst_y = 1;
  size_t img_dtype = CL_FLOAT;
  if (enable_fp16_) {
    img_dtype = CL_HALF_FLOAT;
  }
  std::vector<size_t> img_size{im_dst_x, im_dst_y, img_dtype};
  bias_ = allocator->Malloc(im_dst_x * im_dst_y * C4NUM * data_size, img_size);
  bias_ = allocator->MapBuffer(bias_, CL_MAP_WRITE, nullptr, true);
  memset(bias_, 0x00, div_co * C4NUM * data_size);
  if (in_tensors_.size() >= 3) {
    auto bias_dtype = in_tensors_[2]->data_type();
    if (bias_dtype == kNumberTypeFloat32 && enable_fp16_) {
      for (int i = 0; i < co; i++) {
        reinterpret_cast<float16_t *>(bias_)[i] = reinterpret_cast<float *>(in_tensors_[2]->data_c())[i];
      }
    } else if (bias_dtype == kNumberTypeFloat16 && !enable_fp16_) {
      for (int i = 0; i < co; i++) {
        reinterpret_cast<float *>(bias_)[i] = reinterpret_cast<float16_t *>(in_tensors_[2]->data_c())[i];
      }
    } else {
      memcpy(bias_, in_tensors_[2]->data_c(), co * data_size);
    }
  }
  allocator->UnmapBuffer(bias_);
}

int Conv2dTransposeOpenCLKernel::GetImageSize(size_t idx, std::vector<size_t> *img_size) {
  size_t im_dst_x, im_dst_y;
  int n = out_tensors_[0]->shape()[0];
  int h = out_tensors_[0]->shape()[1];
  int w = out_tensors_[0]->shape()[2];
  int c = out_tensors_[0]->shape()[3];
  if (op_format_ == schema::Format::Format_NHWC4) {
    im_dst_x = w * UP_DIV(c, C4NUM);
    im_dst_y = n * h;
  } else if (op_format_ == schema::Format::Format_NC4HW4) {
    im_dst_x = w;
    im_dst_y = n * UP_DIV(c, C4NUM) * h;
  } else {
    MS_LOG(ERROR) << "not support op format:" << EnumNameFormat(op_format_);
    return RET_ERROR;
  }
  size_t img_dtype = CL_FLOAT;
  if (enable_fp16_) {
    img_dtype = CL_HALF_FLOAT;
  }
  img_size->clear();
  std::vector<size_t> vec{im_dst_x, im_dst_y, img_dtype};
  *img_size = vec;
  return RET_OK;
}

int Conv2dTransposeOpenCLKernel::Run() {
  MS_LOG(DEBUG) << this->name() << " Running!";
  ConvParameter *param = reinterpret_cast<ConvParameter *>(op_parameter_);
  int ci = in_tensors_[0]->shape()[3];
  int co = out_tensors_[0]->shape()[3];
  int co4 = UP_DIV(co, C4NUM);
  int kh = param->kernel_h_;
  int kw = param->kernel_w_;
  int pad = param->pad_u_;
  int oh = out_tensors_[0]->shape()[1];
  int ow = out_tensors_[0]->shape()[2];
  int h = in_tensors_[0]->shape()[1];
  int w = in_tensors_[0]->shape()[2];
  auto ocl_runtime = lite::opencl::OpenCLRuntime::GetInstance();
  // local size should less than MAX_GROUP_SIZE
  std::vector<size_t> local = {16, 1, 16};
  std::vector<size_t> global = {UP_ROUND((size_t)UP_ROUND(oh / 2, 2), local[0]),
                                UP_ROUND((size_t)UP_ROUND(ow / 2, 2), local[1]), UP_ROUND(co4, local[2])};

  cl_int2 kernel_size = {kh, kw};
  cl_int2 stride = {2, 2};
  cl_int2 padding = {pad, pad};
  cl_int4 src_size = {h, w, UP_DIV(ci, C4NUM), 1};
  cl_int4 dst_size = {oh, ow, UP_DIV(co, C4NUM), 1};
  int arg_cnt = 0;
  ocl_runtime->SetKernelArg(kernel_, arg_cnt++, in_tensors_[0]->data_c());
  ocl_runtime->SetKernelArg(kernel_, arg_cnt++, padWeight_, lite::opencl::MemType::BUF);
  ocl_runtime->SetKernelArg(kernel_, arg_cnt++, bias_);
  ocl_runtime->SetKernelArg(kernel_, arg_cnt++, out_tensors_[0]->data_c());
  ocl_runtime->SetKernelArg(kernel_, arg_cnt++, kernel_size);
  ocl_runtime->SetKernelArg(kernel_, arg_cnt++, stride);
  ocl_runtime->SetKernelArg(kernel_, arg_cnt++, padding);
  ocl_runtime->SetKernelArg(kernel_, arg_cnt++, src_size);
  ocl_runtime->SetKernelArg(kernel_, arg_cnt++, dst_size);
  ocl_runtime->RunKernel(kernel_, global, local, nullptr);
  return RET_OK;
}

kernel::LiteKernel *OpenCLConv2dTransposeKernelCreator(const std::vector<lite::Tensor *> &inputs,
                                                       const std::vector<lite::Tensor *> &outputs,
                                                       OpParameter *opParameter, const lite::Context *ctx,
                                                       const kernel::KernelKey &desc,
                                                       const mindspore::lite::PrimitiveC *primitive) {
  auto *kernel =
    new (std::nothrow) Conv2dTransposeOpenCLKernel(reinterpret_cast<OpParameter *>(opParameter), inputs, outputs);
  if (kernel == nullptr) {
    MS_LOG(ERROR) << "kernel " << opParameter->name_ << "is nullptr.";
    return nullptr;
  }
  auto ret = kernel->Init();
  if (ret != RET_OK) {
    delete kernel;
    return nullptr;
  }
  return kernel;
}

REG_KERNEL(kGPU, kNumberTypeFloat32, PrimitiveType_DeConv2D, OpenCLConv2dTransposeKernelCreator)
REG_KERNEL(kGPU, kNumberTypeFloat16, PrimitiveType_DeConv2D, OpenCLConv2dTransposeKernelCreator)
}  // namespace mindspore::kernel
