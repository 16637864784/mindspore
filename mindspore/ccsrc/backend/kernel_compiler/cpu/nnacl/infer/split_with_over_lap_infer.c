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

#include "nnacl/infer/split_with_over_lap_infer.h"
#include "nnacl/infer/infer_register.h"

int SplitWithOverlapInferShape(const TensorC *const *inputs, size_t inputs_size, TensorC **outputs, size_t outputs_size,
                               OpParameter *parameter) {
#ifdef Debug
  int check_ret = CheckAugmentNull(inputs, inputs_size, outputs, outputs_size, parameter);
  if (check_ret != NNACL_OK) {
    return check_ret;
  }
#endif
  if (!parameter->infer_flag_) {
    return NNACL_INFER_INVALID;
  }
  const TensorC *input = inputs[0];
  if (inputs_size < 1) {
    return NNACL_ERR;
  }
  if (outputs_size == 0) {
    return NNACL_ERR;
  }
  SplitWithOverlapParameter *param = (SplitWithOverlapParameter *)parameter;
  int number_split = param->num_split_;
  if (outputs_size != number_split) {
    return NNACL_ERR;
  }
  int stride = param->stride_;
  int pad_top = param->pad_top_;
  int split_dim = param->split_dim_;

  int ratio[SPLIT_MAX_SLICE_NUM];
  int extend_top[SPLIT_MAX_SLICE_NUM];
  int extend_bottom[SPLIT_MAX_SLICE_NUM];
  for (int i = 0; i < number_split; ++i) {
    ratio[i] = param->ratio_[i];
    extend_top[i] = param->extend_top_[i];
    extend_bottom[i] = param->extend_bottom_[i];
  }

  const int *input_shape = input->shape_;
  int split_dim_size = input_shape[split_dim];
  int total_block_count = 0;
  for (int i = 0; i < number_split; i++) {
    total_block_count += ratio[i];
  }

  int borders[MAX_SHAPE_SIZE];
  borders[0] = 0;
  int visited_block = 0;
  for (int i = 0; i < number_split - 1; i++) {
    visited_block += ratio[i];

    int cur_border = UP_DIV(split_dim_size * visited_block, total_block_count);
    if (stride != 0) {
      // make sure border align with stride
      cur_border = UP_ROUND(cur_border + pad_top, stride);
      borders[i + 1] = cur_border - pad_top;
    } else {
      borders[i + 1] = cur_border;
    }
  }
  borders[number_split - 1] = split_dim_size;

  for (int i = 0; i < number_split; ++i) {
    int output_shape[MAX_SHAPE_SIZE];
    for (int dim = 0; dim < input->shape_size_; dim++) {
      if (dim == split_dim) {
        int splited_size = borders[i + 1] - borders[i];
        splited_size += extend_top[i] + extend_bottom[i];
        output_shape[dim] = splited_size;
      } else {
        output_shape[dim] = input_shape[dim];
      }
    }
    SetShapeArray(outputs[i], output_shape, input->shape_size_);
    SetDataTypeFormat(outputs[i], input);
  }
  return NNACL_OK;
}

REG_INFER(SplitWithOverlap, PrimType_SplitWithOverlap, SplitWithOverlapInferShape)
