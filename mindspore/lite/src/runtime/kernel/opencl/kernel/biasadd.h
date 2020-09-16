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

#ifndef MINDSPORE_LITE_SRC_RUNTIME_KERNEL_OPENCL_KERNEL_BIASADD_H_
#define MINDSPORE_LITE_SRC_RUNTIME_KERNEL_OPENCL_KERNEL_BIASADD_H_

#include <vector>
#include <string>

#include "src/tensor.h"
#include "src/runtime/kernel/opencl/opencl_kernel.h"
#include "schema/model_generated.h"

namespace mindspore::kernel {

class BiasAddOpenCLKernel : public OpenCLKernel {
 public:
  explicit BiasAddOpenCLKernel(OpParameter *parameter, const std::vector<lite::Tensor *> &inputs,
                               const std::vector<lite::Tensor *> &outputs)
      : OpenCLKernel(parameter, inputs, outputs) {}
  ~BiasAddOpenCLKernel() override{};

  int Init() override;
  int Run() override;
  int GetImageSize(size_t idx, std::vector<size_t> *img_size) override;
  void InitBuffer();
  cl_int4 GetGlobalshape();

 private:
  cl::Kernel kernel_;
  void *BiasAdd_;
  int in_size_;
  int out_size_;
  size_t fp_size;
  cl_int4 input_shape_;
  bool enable_fp16_{false};
};

}  // namespace mindspore::kernel

#endif  // MINDSPORE_LITE_SRC_RUNTIME_KERNEL_OPENCL_KERNEL_BIASADD_H_
