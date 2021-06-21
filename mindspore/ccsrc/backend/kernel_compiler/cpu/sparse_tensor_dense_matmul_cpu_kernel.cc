/**
 * Copyright 2020-2021 Huawei Technologies Co., Ltd
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

#include <functional>

#include "backend/kernel_compiler/cpu/sparse_tensor_dense_matmul_cpu_kernel.h"

namespace mindspore {
namespace kernel {
template <typename I, typename T>
void SparseTensorDenseMatmulCPUKernel<I, T>::InitKernel(const CNodePtr &kernel_node) {
  adj_st_ = AnfAlgo::GetNodeAttr<bool>(kernel_node, ADJ_ST);
  adj_dt_ = AnfAlgo::GetNodeAttr<bool>(kernel_node, ADJ_dT);
  auto indices_shape = AnfAlgo::GetPrevNodeOutputInferShape(kernel_node, 0);
  if (indices_shape.size() != 2 && indices_shape[1] != 2) {
    MS_LOG(EXCEPTION)
      << "SparseTensorDenseMatmul requires 'indices' should be a 2-D Tensor and the second dimension length "
         "should be 2, but got 'indices' shape: "
      << indices_shape;
  }
  auto values_shape = AnfAlgo::GetPrevNodeOutputInferShape(kernel_node, 1);
  if (values_shape.size() != 1 || values_shape[0] != indices_shape[0]) {
    MS_LOG(EXCEPTION)
      << "SparseTensorDenseMatmul requires 'value's should be a 1-D Tensor and the first dimension length should be "
         "equal to the first dimension length of 'indices', but got 'values' shape: "
      << values_shape;
  }
  output_shape_ = AnfAlgo::GetOutputInferShape(kernel_node, 0);
  values_size_ = values_shape[0];
  b_shape_ = AnfAlgo::GetPrevNodeOutputInferShape(kernel_node, 3);
}

template <typename I, typename T>
bool SparseTensorDenseMatmulCPUKernel<I, T>::Launch(const std::vector<kernel::AddressPtr> &inputs,
                                                    const std::vector<kernel::AddressPtr> & /*workspace*/,
                                                    const std::vector<kernel::AddressPtr> &outputs) {
  if (inputs.size() != 4 || outputs.size() != 1) {
    MS_LOG(ERROR) << "SparseTensorDenseMatmul requires 4 inputs and 1 output, but got " << inputs.size()
                  << " inputs and " << outputs.size() << " output.";
    return false;
  }
  if (outputs[0]->size == 0) {
    MS_LOG(WARNING) << "SparseTensorDenseMatmul output memory size should be greater than 0, but got 0.";
    return true;
  }
  auto a_indices = reinterpret_cast<I *>(inputs[0]->addr);
  auto a_values = reinterpret_cast<T *>(inputs[1]->addr);
  auto b = reinterpret_cast<T *>(inputs[3]->addr);
  auto out = reinterpret_cast<T *>(outputs[0]->addr);
  const size_t indices_length = inputs[0]->size / sizeof(I);
  const size_t values_length = inputs[1]->size / sizeof(T);
  const size_t b_length = inputs[3]->size / sizeof(T);
  if (memset_s(out, outputs[0]->size, 0, outputs[0]->size) != EOK) {
    MS_LOG(EXCEPTION) << "Memset Failed!";
  }

  const size_t out_dim_0 = output_shape_[0];
  const size_t out_dim_1 = output_shape_[1];
  const size_t b_dim_0 = b_shape_[0];
  const size_t b_dim_1 = b_shape_[1];
  const size_t same_dim = adj_dt_ ? b_dim_1 : b_dim_0;
  for (size_t i = 0; i < values_size_; ++i) {
    if (i * 2 + 1 >= indices_length) {
      MS_LOG(EXCEPTION) << "The index of a_indices out of bounds.";
    }
    if (i >= values_length) {
      MS_LOG(EXCEPTION) << "The index of a_values out of bounds.";
    }
    const int row = adj_st_ ? a_indices[i * 2 + 1] : a_indices[i * 2];
    const int col = adj_st_ ? a_indices[i * 2] : a_indices[i * 2 + 1];
    if (row >= SizeToInt(out_dim_0) || row < 0 || col >= SizeToInt(same_dim) || col < 0) {
      MS_EXCEPTION(ValueError) << "The indices including out of bounds index, row range: [0, " << out_dim_0
                               << "), col range: [0, " << same_dim << "), but got row: " << row << ", col: " << col;
    }

    for (size_t n = 0; n < out_dim_1; ++n) {
      if (adj_dt_) {
        if (n * b_dim_1 + col >= b_length) {
          MS_LOG(EXCEPTION) << "The index of b out of bounds.";
        }
        const T b_value = b[n * b_dim_1 + col];
        out[row * out_dim_1 + n] += a_values[i] * b_value;
      } else {
        if (col * b_dim_1 + n >= b_length) {
          MS_LOG(EXCEPTION) << "The index of b out of bounds.";
        }
        const T b_value = b[col * b_dim_1 + n];
        out[row * out_dim_1 + n] += a_values[i] * b_value;
      }
    }
  }
  return true;
}
}  // namespace kernel
}  // namespace mindspore
