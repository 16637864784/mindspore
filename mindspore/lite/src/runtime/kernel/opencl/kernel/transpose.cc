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

#include <set>
#include <string>
#include "include/errorcode.h"
#include "src/kernel_registry.h"
#include "src/runtime/opencl/opencl_runtime.h"
#include "src/runtime/kernel/opencl/kernel/transpose.h"
#ifndef PROGRAM_WITH_IL
#include "src/runtime/kernel/opencl/cl/transpose.cl.inc"
#endif

using mindspore::kernel::KERNEL_ARCH::kGPU;
using mindspore::lite::KernelRegistrar;
using mindspore::lite::RET_ERROR;
using mindspore::lite::RET_OK;
using mindspore::schema::PrimitiveType_Transpose;

namespace mindspore::kernel {

int TransposeOpenCLKernel::Init() {
  std::string kernel_name = "transpose";
  auto ocl_runtime = lite::opencl::OpenCLRuntime::GetInstance();
  enable_fp16_ = ocl_runtime->GetFp16Enable();
  auto param = reinterpret_cast<TransposeParameter *>(op_parameter_);
  if (param->num_axes_ == 4 && param->perm_[0] == 0 && param->perm_[1] == 3 && param->perm_[2] == 1 &&
      param->perm_[3] == 2) {
    type = TransposeType::NHWC2NCHW;
  } else {
    MS_LOG(ERROR) << "unsupported transpose axes.";
    return RET_ERROR;
  }
  out_mem_type_ = OpenCLMemType::BUF;
  kernel_name += "_" + std::string(EnumNameFormat(op_format_));
  if (out_mem_type_ == OpenCLMemType::BUF) {
    kernel_name += "_BUF";
  } else {
    kernel_name += "_IMG";
  }
#ifdef PROGRAM_WITH_IL
  kernel_ = ocl_runtime->GetKernelFromBinary(kernel_name);
#else
  std::set<std::string> build_options;
  std::string source = transpose_source;
  std::string program_name = "transpose";
  ocl_runtime->LoadSource(program_name, source);
  ocl_runtime->BuildKernel(kernel_, program_name, kernel_name, build_options);
#endif
  if ((in_tensors_[0]->shape()[1] * in_tensors_[0]->shape()[2]) % 4 != 0) {
    MS_LOG(ERROR) << "input H * W % 4 != 0 not support!";
    return RET_ERROR;
  }
  in_ori_format_ = in_tensors_[0]->GetFormat();
  out_ori_format_ = out_tensors_[0]->GetFormat();
  in_tensors_[0]->SetFormat(op_format_);
  out_tensors_[0]->SetFormat(op_format_);
  if (out_mem_type_ == OpenCLMemType::BUF) {
    out_ori_format_ = schema::Format::Format_NCHW;
    out_tensors_[0]->SetFormat(schema::Format::Format_NCHW);
  }

  MS_LOG(DEBUG) << kernel_name << " Init Done!";
  return RET_OK;
}

int TransposeOpenCLKernel::ReSize() { return RET_OK; }

int TransposeOpenCLKernel::GetImageSize(size_t idx, std::vector<size_t> *img_size) {
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

int TransposeOpenCLKernel::Run() {
  // notice: input image2d size = {c/4, h * w}
  MS_LOG(DEBUG) << this->name() << " Running!";
  std::vector<int> shapex = in_tensors_[0]->shape();
  int h = shapex[1];
  int w = shapex[2];
  int c = shapex[3];
  int c4 = UP_DIV(c, 4);
  int hw4 = UP_DIV(h * w, 4);
  auto ocl_runtime = lite::opencl::OpenCLRuntime::GetInstance();
  std::vector<size_t> local = {16, 16};
  std::vector<size_t> global = {UP_ROUND(hw4, local[0]), UP_ROUND(c4, local[1])};

  cl_int2 HW = {h * w, hw4};
  cl_int2 C = {c, c4};
  int arg_idx = 0;
  ocl_runtime->SetKernelArg(kernel_, arg_idx++, in_tensors_[0]->data_c());
  if (out_mem_type_ == OpenCLMemType::BUF) {
    ocl_runtime->SetKernelArg(kernel_, arg_idx++, out_tensors_[0]->data_c(), lite::opencl::MemType::BUF);
  } else {
    ocl_runtime->SetKernelArg(kernel_, arg_idx++, out_tensors_[0]->data_c());
  }
  ocl_runtime->SetKernelArg(kernel_, arg_idx++, HW);
  ocl_runtime->SetKernelArg(kernel_, arg_idx++, C);
  ocl_runtime->SetKernelArg(kernel_, arg_idx++, w);
  ocl_runtime->SetKernelArg(kernel_, arg_idx++, h);
  ocl_runtime->RunKernel(kernel_, global, local, nullptr);
  return RET_OK;
}

kernel::LiteKernel *OpenCLTransposeKernelCreator(const std::vector<lite::Tensor *> &inputs,
                                                 const std::vector<lite::Tensor *> &outputs, OpParameter *opParameter,
                                                 const lite::Context *ctx, const kernel::KernelKey &desc,
                                                 const mindspore::lite::PrimitiveC *primitive) {
  auto *kernel =
    new (std::nothrow) TransposeOpenCLKernel(reinterpret_cast<OpParameter *>(opParameter), inputs, outputs);
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

REG_KERNEL(kGPU, kNumberTypeFloat32, PrimitiveType_Transpose, OpenCLTransposeKernelCreator)
REG_KERNEL(kGPU, kNumberTypeFloat16, PrimitiveType_Transpose, OpenCLTransposeKernelCreator)
}  // namespace mindspore::kernel
