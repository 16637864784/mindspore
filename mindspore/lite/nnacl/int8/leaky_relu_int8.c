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

#include "nnacl/int8/leaky_relu_int8.h"
#include "nnacl/errorcode.h"

int DoLeakReluInt8(int8_t *inputs, int8_t *output_ptr, LeakyReluQuantArg *quant_prelu_parm, QuantArg *input_quant,
                   int task_id) {
  if (quant_prelu_parm == NULL) {
    return NNACL_NULL_PTR;
  }
  float output_scale = quant_prelu_parm->quant_arg.out_args_.scale_;
  int output_zp = quant_prelu_parm->quant_arg.out_args_.zp_;
  const float output_inverse_scale = 1.f / output_scale;
  int output_dim = quant_prelu_parm->input_dim_;

  for (int i = 0; i < output_dim; i++) {
    input_quant[i].scale_ = quant_prelu_parm->quant_arg.in_args_.scale_;
    input_quant[i].zp_ = quant_prelu_parm->quant_arg.in_args_.zp_;
  }

  for (int i = 0; i < output_dim; i++) {
    float scale = input_quant[i].scale_ * output_inverse_scale;
    float bias = -input_quant[i].zp_ * scale;
    for (int j = task_id; j < quant_prelu_parm->element_num; j += quant_prelu_parm->op_parameter_.thread_num_) {
      if (inputs[j] <= 0) {
        int32_t output_tmp = round(inputs[j] * quant_prelu_parm->slope_[0] * scale + bias) + output_zp;
        if (output_tmp > 127) {
          output_ptr[j] = 127;
        } else if (output_tmp < -128) {
          output_ptr[j] = -128;
        } else {
          output_ptr[j] = (int8_t)output_tmp;
        }
      } else {
        int32_t output_tmp = round(inputs[j] * scale + bias) + output_zp;
        if (output_tmp > 127) {
          output_ptr[j] = 127;
        } else if (output_tmp < -128) {
          output_ptr[j] = -128;
        } else {
          output_ptr[j] = (int8_t)output_tmp;
        }
      }
    }
  }
  free(input_quant);
  return NNACL_OK;
}
