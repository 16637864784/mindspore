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
#ifndef MINDSPORE_LITE_NNACL_FP16_CONV_FP16_H_
#define MINDSPORE_LITE_NNACL_FP16_CONV_FP16_H_

#include <arm_neon.h>
#include "nnacl/conv_parameter.h"
#include "nnacl/fp16/winograd_utils_fp16.h"
#include "nnacl/fp16/winograd_transform_fp16.h"

typedef float16_t *TmpBufferAddressFp16;
typedef float16_t *MatricesFp16;

#ifndef ENABLE_NEON
void IndirectGemmFp16_16x8(float16_t *output, float16_t *input, float16_t *weight, float16_t *bias, size_t step,
                           size_t ic4, size_t oc8, size_t offset, size_t mode, size_t writeC8, size_t relu,
                           size_t relu6);

void IndirectGemmFp16_16x8_common(float16_t *output, float16_t *input, float16_t *weight, float16_t *bias, size_t step,
                                  size_t ic4, size_t oc8, size_t offset, size_t relu, size_t relu6);

void IndirectGemmFp16_16x8_c8(float16_t *output, float16_t *input, float16_t *weight, float16_t *bias, size_t step,
                              size_t ic4, size_t oc8, size_t offset, size_t mode, size_t writeC8, size_t relu,
                              size_t relu6);
#endif

#ifdef __cplusplus
extern "C" {
#endif

// fp16 convolution common (im2col+gemm)
void ConvFp16(float16_t *input_data, float16_t *packed_input, float16_t *packed_weight, float16_t *bias_data,
              float16_t *tmp_out_block, float16_t *output_data, int task_id, ConvParameter *conv_param);

// fp16 conv3x3
void Conv3x3Fp16(float16_t *input_data, float16_t *transed_weight, const float16_t *bias_data, float16_t *output_data,
                 float16_t *tile_buffer, float16_t *block_unit_buffer, float16_t *tmp_dst_buffer, float16_t *tmp_out,
                 int task_id, ConvParameter *conv_param);

void UnPack3x3OutputFp16(const float16_t *src, float16_t *dst, int batch, int height, int width, int channel);

void UnPack3x3ReluOutputFp16(const float16_t *src, float16_t *dst, int batch, int height, int width, int channel);

void UnPack3x3Relu6OutputFp16(const float16_t *src, float16_t *dst, int batch, int height, int width, int channel);

// fp16 convolution winograd
void ConvWinogardFp16(float16_t *input_data, float16_t *trans_weight, const float16_t *bias_data,
                      float16_t *output_data, TmpBufferAddressFp16 *buffer_list, int task_id, ConvParameter *conv_param,
                      InputTransFp16Func in_func, OutputTransFp16Func out_func);

#ifdef __cplusplus
}
#endif

#endif  // MINDSPORE_LITE_NNACL_FP16_CONV_FP16_H_
