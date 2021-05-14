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
#ifndef MINDSPORE_LITE_SRC_RUNTIME_KERNEL_ARM_BASE_TILE_BASE_H_
#define MINDSPORE_LITE_SRC_RUNTIME_KERNEL_ARM_BASE_TILE_BASE_H_

#include <vector>
#include "src/inner_kernel.h"
#include "nnacl/base/tile_base.h"

namespace mindspore::kernel {
class TileCPUKernel : public InnerKernel {
 public:
  TileCPUKernel(OpParameter *parameter, const std::vector<lite::Tensor *> &inputs,
                const std::vector<lite::Tensor *> &outputs, const lite::InnerContext *ctx)
      : InnerKernel(parameter, inputs, outputs, ctx) {}
  ~TileCPUKernel() override = default;

  int Init() override;
  int ReSize() override;
  int Run() override;
  int SimpleTileImpl(int task_id);

 private:
  int RunSimpleTile();
  void ComputeStrides(const int *shape, int *strides, int ndim);
  void FillOneDimTileParam();
  bool one_dim_tile_ = false;
  uint8_t *input_addr_ = nullptr;
  uint8_t *output_addr_ = nullptr;
  TileParameter *tile_parameter_ = nullptr;
};
}  // namespace mindspore::kernel

#endif  // MINDSPORE_LITE_SRC_RUNTIME_KERNEL_ARM_BASE_TILE_BASE_H_
