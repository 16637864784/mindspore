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

#include "nnacl/int8/common_func_int8.h"
#include "nnacl/quantization/fixed_point.h"

void PostConvFuncCommInt8(const int32_t *in, int8_t *out, const int32_t *bias, size_t oc, size_t plane,
                          size_t out_oc_stride, size_t in_plane_stride, int32_t multiplier, int32_t mini, int32_t maxi,
                          int32_t left_shift, int32_t right_shift, int32_t zp, int size) {
  if (size == 0) {
    return;
  }
  for (int r = 0; r < plane; r++) {
    for (int c = 0; c < oc; c++) {
      int c8div = c / size, c8mod = c % size;
      int src_index = c8div * in_plane_stride + r * size + c8mod;
      int dst_index = r * out_oc_stride + c;
      int32_t value = in[src_index];
      if (bias != NULL) {
        value = in[src_index] + bias[c];
      }
      value = MultiplyByQuantizedMultiplier(value, multiplier, left_shift, right_shift) + zp;
      value = MSMIN(maxi, value);
      value = MSMAX(mini, value);
      out[dst_index] = (int8_t)value;
    }
  }
  return;
}

void PostFuncInt8C4(const int32_t *in, const int32_t *bias, int8_t *out, size_t oc, size_t plane, size_t stride,
                    int32_t multiplier, int32_t left_shift, int32_t right_shift, int32_t zp, int32_t mini,
                    int32_t maxi) {
/*  ((int32_t)row4x4-major + bias) * multiplier + output_zp  =>  (int8)relu  =>  (int8_t)row-major  */
#ifndef ENABLE_ARM64
  PostConvFuncCommInt8(in, out, bias, oc, plane, stride, UP_ROUND(plane, C4NUM) * C4NUM, multiplier, mini, maxi,
                       left_shift, right_shift, zp, C4NUM);
#else
  size_t oc4div = oc / C4NUM * C4NUM;
  size_t oc4res = oc % C4NUM;
  PostFuncInt8C4Neon64(in, bias, out, oc4div, oc4res, plane, stride * sizeof(int8_t), multiplier, left_shift,
                       right_shift, zp, mini, maxi);
#endif
  return;
}
