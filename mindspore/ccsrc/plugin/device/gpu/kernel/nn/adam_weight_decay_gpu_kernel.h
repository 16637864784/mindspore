/**
 * Copyright 2021-2022 Huawei Technologies Co., Ltd
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

#ifndef MINDSPORE_CCSRC_BACKEND_KERNEL_COMPILER_GPU_NN_ADAM_WEIGHT_DECAY_GPU_KERNEL_H_
#define MINDSPORE_CCSRC_BACKEND_KERNEL_COMPILER_GPU_NN_ADAM_WEIGHT_DECAY_GPU_KERNEL_H_

#include <vector>
#include <string>
#include "plugin/device/gpu/kernel/gpu_kernel.h"
#include "plugin/device/gpu/kernel/gpu_kernel_factory.h"
#include "plugin/device/gpu/kernel/cuda_impl/cuda_ops/adam_impl.cuh"
namespace mindspore {
namespace kernel {
constexpr size_t INPUT_NUM = 9;
template <typename T, typename S>
class AdamWeightDecayGpuKernelMod : public DeprecatedNativeGpuKernelMod {
 public:
  AdamWeightDecayGpuKernelMod()
      : variable_size_(0),
        m_size_(0),
        v_size_(0),
        learning_rate_size_(0),
        beta1_size_(0),
        beta2_size_(0),
        epsilon_size_(0),
        decay_size_(0),
        gradient_size_(0),
        is_null_input_(false),
        kernel_name_("AdamWeightDecay") {}

  ~AdamWeightDecayGpuKernelMod() override = default;

  bool Launch(const std::vector<AddressPtr> &inputs, const std::vector<AddressPtr> &, const std::vector<AddressPtr> &,
              void *stream_ptr) override {
    if (is_null_input_) {
      return true;
    }
    S *variable = GetDeviceAddress<S>(inputs, 0);
    T *m = GetDeviceAddress<T>(inputs, 1);
    T *v = GetDeviceAddress<T>(inputs, 2);
    float *lr = GetDeviceAddress<float>(inputs, 3);
    float *beta1 = GetDeviceAddress<float>(inputs, 4);
    float *beta2 = GetDeviceAddress<float>(inputs, 5);
    float *epsilon = GetDeviceAddress<float>(inputs, 6);
    float *decay = GetDeviceAddress<float>(inputs, 7);
    S *gradient = GetDeviceAddress<S>(inputs, 8);
    AdamWeightDecayOp(inputs[0]->size / sizeof(S), gradient, lr, beta1, beta2, epsilon, decay, variable, m, v,
                      reinterpret_cast<cudaStream_t>(stream_ptr));
    return true;
  }

  bool Init(const CNodePtr &kernel_node) override {
    kernel_name_ = common::AnfAlgo::GetCNodeName(kernel_node);
    size_t input_num = common::AnfAlgo::GetInputTensorNum(kernel_node);
    kernel_node_ = kernel_node;
    if (input_num != INPUT_NUM) {
      MS_LOG(EXCEPTION) << "For '" << kernel_name_ << "', the number of inputs must be " << INPUT_NUM << ", but got "
                        << input_num;
    }

    variable_size_ = sizeof(S);
    m_size_ = sizeof(T);
    v_size_ = sizeof(T);
    learning_rate_size_ = sizeof(float);
    beta1_size_ = sizeof(float);
    beta2_size_ = sizeof(float);
    epsilon_size_ = sizeof(float);
    decay_size_ = sizeof(float);
    gradient_size_ = sizeof(S);

    auto variable_shape = common::AnfAlgo::GetPrevNodeOutputInferShape(kernel_node, 0);
    auto m_shape = common::AnfAlgo::GetPrevNodeOutputInferShape(kernel_node, 1);
    auto v_shape = common::AnfAlgo::GetPrevNodeOutputInferShape(kernel_node, 2);
    auto gradient_shape = common::AnfAlgo::GetPrevNodeOutputInferShape(kernel_node, 8);
    is_null_input_ = CHECK_SHAPE_NULL(variable_shape, kernel_name_, "var") ||
                     CHECK_SHAPE_NULL(m_shape, kernel_name_, "m") || CHECK_SHAPE_NULL(v_shape, kernel_name_, "v") ||
                     CHECK_SHAPE_NULL(gradient_shape, kernel_name_, "gradient");
    if (is_null_input_ || AnfAlgo::IsShapesDynamic({variable_shape, m_shape, v_shape, gradient_shape})) {
      InitSizeLists();
      return true;
    }
    variable_size_ *= SizeOf(variable_shape);
    m_size_ *= SizeOf(m_shape);
    v_size_ *= SizeOf(v_shape);
    gradient_size_ *= SizeOf(gradient_shape);

    InitSizeLists();
    return true;
  }

 protected:
  void InitSizeLists() override {
    input_size_list_.push_back(variable_size_);
    input_size_list_.push_back(m_size_);
    input_size_list_.push_back(v_size_);
    input_size_list_.push_back(learning_rate_size_);
    input_size_list_.push_back(beta1_size_);
    input_size_list_.push_back(beta2_size_);
    input_size_list_.push_back(epsilon_size_);
    input_size_list_.push_back(decay_size_);
    input_size_list_.push_back(gradient_size_);
    output_size_list_.push_back(0);
    output_size_list_.push_back(0);
    output_size_list_.push_back(0);
  }

 private:
  size_t variable_size_;
  size_t m_size_;
  size_t v_size_;
  size_t learning_rate_size_;
  size_t beta1_size_;
  size_t beta2_size_;
  size_t epsilon_size_;
  size_t decay_size_;
  size_t gradient_size_;
  bool is_null_input_;
  std::string kernel_name_;
};
}  // namespace kernel
}  // namespace mindspore

#endif  // MINDSPORE_CCSRC_BACKEND_KERNEL_COMPILER_GPU_NN_ADAM_WEIGHT_DECAY_GPU_KERNEL_H_
