/**
 * Copyright 2021 Huawei Technologies Co., Ltd
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

#ifndef MINDSPORE_LOGICAL_NOT_GPU_KERNEL_H
#define MINDSPORE_LOGICAL_NOT_GPU_KERNEL_H
#include <cublas_v2.h>
#include <cuda_runtime_api.h>
#include <vector>
#include <algorithm>
#include <functional>
#include "backend/kernel_compiler/gpu/cuda_impl/logical_not_impl.cuh"
#include "backend/kernel_compiler/gpu/gpu_kernel.h"
#include "backend/kernel_compiler/gpu/gpu_kernel_factory.h"
#include "utils/convert_utils.h"

namespace mindspore {
namespace kernel {
template <typename T>
class LogicalNotGpuKernel : public GpuKernel {
 public:
  LogicalNotGpuKernel() { ResetResource(); }
  ~LogicalNotGpuKernel() override = default;
  const std::vector<size_t> &GetInputSizeList() const override { return input_size_list_; }
  const std::vector<size_t> &GetOutputSizeList() const override { return output_size_list_; }
  const std::vector<size_t> &GetWorkspaceSizeList() const override { return workspace_size_list_; }

  bool Launch(const std::vector<AddressPtr> &inputs, const std::vector<AddressPtr> &workspace,
              const std::vector<AddressPtr> &outputs, void *stream_ptr) override {
    auto input_addr = GetDeviceAddress<T>(inputs, 0);
    auto output_addr = GetDeviceAddress<bool>(outputs, 0);
    LogicalNotImpl(input_num_, input_addr, output_addr, reinterpret_cast<cudaStream_t>(stream_ptr));
    return true;
  }

  bool Init(const CNodePtr &kernel_node) override {
    kernel_node_ = kernel_node;
    auto input_shape = AnfAlgo::GetInputRealDeviceShapeIfExist(kernel_node, 0);
    input_num_ = std::accumulate(input_shape.begin(), input_shape.end(), 1, std::multiplies<size_t>());
    InitSizeLists();
    return true;
  }

  void ResetResource() noexcept override {
    input_num_ = 1;
    input_size_list_.clear();
    output_size_list_.clear();
    workspace_size_list_.clear();
  }

 protected:
  void InitSizeLists() override {
    input_size_list_.push_back(input_num_ * sizeof(T));
    output_size_list_.push_back(input_num_ * sizeof(T));
  }

 private:
  size_t input_num_;
  std::vector<size_t> input_size_list_;
  std::vector<size_t> output_size_list_;
  std::vector<size_t> workspace_size_list_;
};
}  // namespace kernel
}  // namespace mindspore

#endif
