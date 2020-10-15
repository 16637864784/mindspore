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

#include "src/runtime/kernel/opencl/kernel/softmax.h"
#include <string>
#include <set>
#include "include/errorcode.h"
#include "src/kernel_registry.h"
#include "src/runtime/kernel/opencl/utils.h"
#ifndef PROGRAM_WITH_IL
#include "src/runtime/kernel/opencl/cl/softmax.cl.inc"
#endif

using mindspore::kernel::KERNEL_ARCH::kGPU;
using mindspore::lite::KernelRegistrar;
using mindspore::schema::PrimitiveType_SoftMax;

namespace mindspore::kernel {

std::vector<float> SoftmaxOpenCLKernel::GetMaskForLastChannel(int channels) {
  std::vector<float> mask{0.0f, 0.0f, 0.0f, 0.0f};
  const int reminder = channels % 4 == 0 ? 4 : channels % 4;
  for (int i = 0; i < reminder; ++i) {
    mask[i] = 1.0f;
  }
  return mask;
}

int SoftmaxOpenCLKernel::InitGlobalSize() {
  const size_t global_x = out_tensors_[0]->shape()[1];
  const size_t global_y = out_tensors_[0]->shape()[2];
  const size_t global_z = 1;
  global_size_ = {global_x, global_y, global_z};
  return lite::RET_OK;
}

int SoftmaxOpenCLKernel::SetWorkGroupSize() {
  // set work group size
  InitGlobalSize();
  int max_work_group_size = ocl_runtime_->GetKernelMaxWorkGroupSize(kernel_(), (*ocl_runtime_->Device())());
  local_size_ = GetCommonLocalSize(global_size_, max_work_group_size);
  global_size_ = GetCommonGlobalSize(local_size_, global_size_);
  return lite::RET_OK;
}

int SoftmaxOpenCLKernel::SetWorkGroupSize1x1() {
  local_size_ = {32, 1, 1};
  global_size_ = {32, 1, 1};
  return lite::RET_OK;
}

int SoftmaxOpenCLKernel::GetImageSize(size_t idx, std::vector<size_t> *img_size) {
  size_t im_dst_x, im_dst_y;
  auto out_shape = out_tensors_[0]->shape();
  int n = 1, h = 1, w = 1, c = 1;
  if (out_shape.size() == 2) {
    n = out_shape[0];
    c = out_shape[1];
  } else if (out_shape.size() == 4) {
    n = out_shape[0];
    h = out_shape[1];
    w = out_shape[2];
    c = out_shape[3];
  }
  if (op_format_ == schema::Format_NHWC4) {
    im_dst_x = w * UP_DIV(c, C4NUM);
    im_dst_y = n * h;
  } else if (op_format_ == schema::Format_NC4HW4) {
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

int SoftmaxOpenCLKernel::Init() {
  std::string kernel_name = "SoftMax";
  std::string program_name = "SoftMax";

  std::string source = softmax_source;
  enable_fp16_ = ocl_runtime_->GetFp16Enable();
  // framework not set this param yet! just use default.
  if (in_tensors_[0]->shape().size() == 4) {
    // support 4d tensor
    onexone_flag_ = false;
  } else if (in_tensors_[0]->shape().size() == 2) {
    // support 2d tensor
    kernel_name += "1x1";
    program_name += "1x1";
    onexone_flag_ = true;
  } else {
    MS_LOG(ERROR) << "Init `Softmax` kernel failed: Unsupported shape size: " << in_tensors_[0]->shape().size();
    return RET_ERROR;
  }
  kernel_name += "_" + std::string(EnumNameFormat(op_format_));
#ifdef PROGRAM_WITH_IL
  kernel_ = ocl_runtime->GetKernelFromBinary(kernel_name);
#else
  if (!is_image_out_) {
    out_mem_type_ = OpenCLMemType::BUF;
  } else {
    out_mem_type_ = OpenCLMemType::IMG;
  }
  if (out_mem_type_ == OpenCLMemType::BUF) {
    kernel_name += "_BUF";
    program_name += "_BUF";
  } else {
    kernel_name += "_IMG";
    program_name += "_IMG";
  }
  std::set<std::string> build_options;
  ocl_runtime_->LoadSource(program_name, source);
  ocl_runtime_->BuildKernel(kernel_, program_name, kernel_name, build_options);
#endif
  in_ori_format_ = in_tensors_[0]->GetFormat();
  out_ori_format_ = out_tensors_[0]->GetFormat();
  in_tensors_[0]->SetFormat(op_format_);
  out_tensors_[0]->SetFormat(op_format_);
  if (!is_image_out_) {
    out_tensors_[0]->SetFormat(out_ori_format_);
  }
  MS_LOG(DEBUG) << kernel_name << " Init Done!";
  return lite::RET_OK;
}

int SoftmaxOpenCLKernel::Run() {
  MS_LOG(DEBUG) << this->name() << " Running!";

  int arg_idx = 0;
  if (onexone_flag_) {
    int channel_size = in_tensors_[0]->shape()[1];
    int slices = UP_DIV(channel_size, C4NUM);
    cl_int slices_x32 = UP_DIV(slices, 32);
    auto mask_ = GetMaskForLastChannel(channel_size);
    cl_float4 mask = {mask_[0], mask_[1], mask_[2], mask_[3]};

    ocl_runtime_->SetKernelArg(kernel_, arg_idx++, in_tensors_[0]->data_c());
    if (is_image_out_) {
      ocl_runtime_->SetKernelArg(kernel_, arg_idx++, out_tensors_[0]->data_c());
    } else {
      ocl_runtime_->SetKernelArg(kernel_, arg_idx++, out_tensors_[0]->data_c(), lite::opencl::MemType::BUF);
    }
    ocl_runtime_->SetKernelArg(kernel_, arg_idx++, mask);
    ocl_runtime_->SetKernelArg(kernel_, arg_idx++, slices);
    ocl_runtime_->SetKernelArg(kernel_, arg_idx, slices_x32);
    SetWorkGroupSize1x1();
  } else {
    int slices = UP_DIV(out_tensors_[0]->shape()[3], C4NUM);
    cl_int4 input_shape = {in_tensors_[0]->shape()[1], in_tensors_[0]->shape()[2], in_tensors_[0]->shape()[3], slices};

    ocl_runtime_->SetKernelArg(kernel_, arg_idx++, in_tensors_[0]->data_c());
    if (is_image_out_) {
      ocl_runtime_->SetKernelArg(kernel_, arg_idx++, out_tensors_[0]->data_c());
    } else {
      ocl_runtime_->SetKernelArg(kernel_, arg_idx++, out_tensors_[0]->data_c(), lite::opencl::MemType::BUF);
    }
    ocl_runtime_->SetKernelArg(kernel_, arg_idx, input_shape);
    SetWorkGroupSize();
  }

  // run opengl kernel
  ocl_runtime_->RunKernel(kernel_, global_size_, local_size_, nullptr);
  return lite::RET_OK;
}

kernel::LiteKernel *OpenCLSoftMaxKernelCreator(const std::vector<lite::Tensor *> &inputs,
                                               const std::vector<lite::Tensor *> &outputs, OpParameter *opParameter,
                                               const lite::InnerContext *ctx, const kernel::KernelKey &desc,
                                               const mindspore::lite::PrimitiveC *primitive) {
  auto *kernel = new (std::nothrow) SoftmaxOpenCLKernel(reinterpret_cast<OpParameter *>(opParameter), inputs, outputs);
  if (kernel == nullptr) {
    MS_LOG(ERROR) << "kernel " << opParameter->name_ << "is nullptr.";
    free(opParameter);
    delete kernel;
    return nullptr;
  }
  if (inputs[0]->shape()[0] > 1) {
    MS_LOG(ERROR) << "Init `Softmax` kernel failed: Unsupported multi-batch.";
    delete kernel;
    return nullptr;
  }
  auto ret = kernel->Init();
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "Init `Softmax` kernel failed!";
    delete kernel;
    return nullptr;
  }
  return kernel;
}

REG_KERNEL(kGPU, kNumberTypeFloat32, PrimitiveType_SoftMax, OpenCLSoftMaxKernelCreator)
REG_KERNEL(kGPU, kNumberTypeFloat16, PrimitiveType_SoftMax, OpenCLSoftMaxKernelCreator)
}  // namespace mindspore::kernel
