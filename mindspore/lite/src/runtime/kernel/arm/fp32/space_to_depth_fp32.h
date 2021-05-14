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
#ifndef MINDSPORE_LITE_SRC_BACKEND_ARM_FP32_SPACE_TO_DEPTH_H_
#define MINDSPORE_LITE_SRC_BACKEND_ARM_FP32_SPACE_TO_DEPTH_H_

#include <vector>
#include "src/inner_kernel.h"

namespace mindspore::kernel {
class SpaceToDepthCPUKernel : public InnerKernel {
 public:
  SpaceToDepthCPUKernel(OpParameter *parameter, const std::vector<lite::Tensor *> &inputs,
                        const std::vector<lite::Tensor *> &outputs, const lite::InnerContext *ctx)
      : InnerKernel(parameter, inputs, outputs, ctx) {}
  ~SpaceToDepthCPUKernel() = default;

  int SpaceToDepth(int task_id);
  int Init() override;
  int ReSize() override;
  int Run() override;

 private:
  int thread_h_stride_ = 0;
  int thread_h_num_ = 0;
  int num_unit_ = 0;
  float *input_ptr_ = nullptr;
  float *output_ptr_ = nullptr;
};
}  // namespace mindspore::kernel

#endif  // MINDSPORE_LITE_SRC_BACKEND_ARM_FP32_SPACE_TO_DEPTH_H_
