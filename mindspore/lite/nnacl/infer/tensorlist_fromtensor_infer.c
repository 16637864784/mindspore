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

#include "nnacl/infer/tensorlist_fromtensor_infer.h"

int TensorListFromTensorInferShape(const TensorC *const *inputs, size_t inputs_size, TensorC **outputs,
                                   size_t outputs_size, OpParameter *parameter) {
  int check_ret = CheckAugmentNull(inputs, inputs_size, outputs, outputs_size, parameter);
  if (check_ret != NNACL_OK) {
    return check_ret;
  }
  if (!parameter->infer_flag_) {
    return NNACL_INFER_INVALID;
  }
  const TensorC *input0 = inputs[0];

  if (input0->shape_size_ < 1) {
    return NNACL_ERR;
  }
  int dim0 = input0->shape_[0];
  if (dim0 < 0) {
    return NNACL_ERR;
  }
  const TensorC *input1 = inputs[1];
  if (input1->data_ == NULL) {
    return NNACL_NULL_PTR;
  }
  int *ele_shape_ptr = (int *)(input1->data_);
  TensorListC *output = (TensorListC *)(outputs[0]);
  vvector *tensor_shape = (vvector *)malloc(sizeof(vvector));
  tensor_shape->size_ = dim0;
  for (size_t i = 0; i < dim0; i++) {
    tensor_shape->shape_[i] = (int *)(input0->shape_ + 1);
    tensor_shape->shape_size_[i] = input0->shape_size_ - 1;
  }

  ShapeSet(output->element_shape_, &(output->element_shape_size_), ele_shape_ptr, GetElementNum(input1));
  output->element_num_ = dim0;
  output->data_type_ = kObjectTypeTensorType;
  MallocTensorListData(output, input0->data_type_, tensor_shape);
  free(tensor_shape);
  return NNACL_OK;
}
