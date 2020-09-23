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

#include "nnacl/fp16/pack_fp16.h"
#include <string.h>
#include <stdlib.h>

void Conv1x1InputPackFp16(const float16_t *src, float16_t *dst, ConvParameter *conv_param) {
  /* support nhwc */
  for (int dst_h = 0; dst_h < conv_param->output_h_; dst_h++) {
    int src_h = dst_h * conv_param->stride_h_ - conv_param->pad_u_;
    if (src_h < 0 || src_h >= conv_param->input_h_) {
      continue;
    }
    const float16_t *src_h_ptr = src + src_h * conv_param->input_w_ * conv_param->input_channel_;
    float16_t *dst_h_ptr = dst + dst_h * conv_param->output_w_ * conv_param->input_channel_;
    for (int dst_w = 0; dst_w < conv_param->output_w_; dst_w++) {
      int src_w = dst_w * conv_param->stride_w_ - conv_param->pad_l_;
      if (src_w < 0 || src_w >= conv_param->input_w_) {
        continue;
      }
      memcpy(dst_h_ptr + dst_w * conv_param->input_channel_, src_h_ptr + src_w * conv_param->input_channel_,
             conv_param->input_channel_ * sizeof(float16_t));
    }
  }
  return;
}

void Im2ColPackUnitFp16(float16_t *input_data, ConvParameter *conv_param, float16_t *packed_input, int real_cal_num,
                        int block_index) {
  // input format : nhwc
  int kernel_h = conv_param->kernel_h_;
  int kernel_w = conv_param->kernel_w_;
  int stride_h = conv_param->stride_h_;
  int stride_w = conv_param->stride_w_;
  int pad_h = conv_param->pad_u_;
  int pad_w = conv_param->pad_l_;
  int dilation_h = conv_param->dilation_h_;
  int dilation_w = conv_param->dilation_w_;
  int in_channel = conv_param->input_channel_;
  int in_h = conv_param->input_h_;
  int in_w = conv_param->input_w_;
  int out_w = conv_param->output_w_;
  int ic4 = UP_DIV(in_channel, 4);
  int ic4_minus = in_channel / 4;
  memset(packed_input, 0, kernel_w * kernel_h * ic4 * C4NUM * 16 * sizeof(float16_t));

  for (int i = 0; i < real_cal_num; i++) {
    int block_start = block_index + i;
    int input_h = block_start / out_w * stride_h - pad_h;
    int input_w = block_start % out_w * stride_w - pad_w;
    int input_stride = (input_h * in_w + input_w) * in_channel;
    int kh_s = MSMAX(0, UP_DIV(-input_h, dilation_h));
    int kh_e = MSMIN(kernel_h, UP_DIV(in_h - input_h, dilation_h));
    int kw_s = MSMAX(0, UP_DIV(-input_w, dilation_w));
    int kw_e = MSMIN(kernel_w, UP_DIV(in_w - input_w, dilation_w));
    for (int j = kh_s; j < kh_e; j++) {
      int input_y_stride = j * dilation_h * in_w * in_channel + input_stride;
      for (int n = kw_s; n < kw_e; n++) {
        int input_x_stride = input_y_stride + n * dilation_w * in_channel;
        int input_plane_offset = (j * kernel_w + n) * 16 * C4NUM * ic4 + i * C4NUM;
        for (int m = 0; m < ic4_minus; m++) {
          int channel_block_stride = input_x_stride + m * C4NUM;
          int channel_block_offset = input_plane_offset + m * 16 * C4NUM;
#ifdef ENABLE_ARM64
          vst1_f16(packed_input + channel_block_offset, vld1_f16(input_data + channel_block_stride));
#else
          for (int l = 0; l < C4NUM; ++l) {
            (packed_input + channel_block_offset)[l] = (input_data + channel_block_stride)[l];
          }
#endif
        }  // channel_block loop
        int ic_res = in_channel - ic4_minus * C4NUM;
        for (int l = 0; l < ic_res; ++l) {
          int channel_block_stride = input_x_stride + ic4_minus * C4NUM + l;
          int channel_block_offset = input_plane_offset + ic4_minus * 16 * C4NUM + l;
          packed_input[channel_block_offset] = input_data[channel_block_stride];
        }
      }  // kernel_w loop
    }    // kernel_h loop
  }      // tile num loop
}

void PackWeightFp16(float16_t *weight_data, ConvParameter *conv_param, float16_t *packed_weight) {
  // original weight format : ohwi
  const int tile_num = 8;
  const int inchannel_block = 4;
  int kernel_h = conv_param->kernel_h_;
  int kernel_w = conv_param->kernel_w_;
  int in_channel = conv_param->input_channel_;
  int out_channel = conv_param->output_channel_;
  int kernel_block = UP_DIV(out_channel, tile_num);
  int channel_block = UP_DIV(in_channel, inchannel_block);
  int kernel_plane = kernel_h * kernel_w;
  int pack_weight_size = kernel_block * channel_block * tile_num * inchannel_block * kernel_plane;

  int unit_size = tile_num * inchannel_block;
  int block_size = pack_weight_size / kernel_block;

  for (int m = 0; m < kernel_plane; m++) {
    int kernel_plane_stride = m * in_channel;
    int packed_kernel_plane_stride = m * unit_size * channel_block;
    for (int i = 0; i < channel_block; i++) {
      int channel_block_stride = kernel_plane_stride + i * inchannel_block;
      int packed_channel_block_size = packed_kernel_plane_stride + i * unit_size;
      int ic_remainder = in_channel - i * inchannel_block;
      int real_ic_num = ic_remainder < inchannel_block ? ic_remainder : inchannel_block;
      for (int h = 0; h < real_ic_num; h++) {
        int block_stride = channel_block_stride + h;
        int packed_block_stride = packed_channel_block_size + h * tile_num;
        for (int j = 0; j < kernel_block; j++) {
          int kernel_block_stride = block_stride + j * tile_num * kernel_plane * in_channel;
          int packed_kernel_block_size = packed_block_stride + j * block_size;
          int oc_remainder = out_channel - j * tile_num;
          int real_oc_num = oc_remainder < tile_num ? oc_remainder : tile_num;
          for (int k = 0; k < real_oc_num; k++) {
            float16_t *origin_data_ptr = weight_data + kernel_block_stride + k * kernel_plane * in_channel;
            float16_t *packed_data_ptr = packed_weight + packed_kernel_block_size + k;
            *packed_data_ptr = *origin_data_ptr;
          }
        }  // kernel block loop
      }    // inchannel block loop
    }      // channel block loop
  }        // kernel plane loop
}

void PackWeightToC8Fp16(const float16_t *origin_weight_data, float16_t *packed_weight_data, ConvParameter *conv_param) {
  // origin weight format : ohwi
  int input_channel = conv_param->input_channel_;
  int ic8 = UP_DIV(input_channel, C8NUM);
  int output_channel = conv_param->output_channel_;
  int kernel_plane = conv_param->kernel_h_ * conv_param->kernel_w_;

  for (int k = 0; k < kernel_plane; k++) {
    int src_kernel_offset = k * input_channel;
    int dst_kernel_offset = k * C8NUM;
    for (int o = 0; o < output_channel; o++) {
      int src_oc_offset = src_kernel_offset + o * kernel_plane * input_channel;
      int dst_oc_offset = dst_kernel_offset + o * ic8 * kernel_plane * C8NUM;
      for (int i = 0; i < input_channel; i++) {
        int c8_block_num = i / C8NUM;
        int c8_block_rem = i % C8NUM;
        int src_ic_offset = src_oc_offset + i;
        int dst_ic_offset = dst_oc_offset + c8_block_num * kernel_plane * C8NUM + c8_block_rem;
        (packed_weight_data + dst_ic_offset)[0] = (origin_weight_data + src_ic_offset)[0];
      }
    }
  }
}

void PackWeightToC4Fp16(const float16_t *origin_weight_data, float16_t *packed_weight_data, ConvParameter *conv_param) {
  // origin weight format : ohwi
  int input_channel = conv_param->input_channel_;
  int ic8 = UP_DIV(input_channel, C8NUM);
  int ic4 = ic8 * 2;
  int output_channel = conv_param->output_channel_;
  int kernel_plane = conv_param->kernel_h_ * conv_param->kernel_w_;

  for (int k = 0; k < kernel_plane; k++) {
    int src_kernel_offset = k * input_channel;
    int dst_kernel_offset = k * C4NUM;
    for (int o = 0; o < output_channel; o++) {
      int src_oc_offset = src_kernel_offset + o * kernel_plane * input_channel;
      int dst_oc_offset = dst_kernel_offset + o * ic4 * kernel_plane * C4NUM;
      for (int i = 0; i < input_channel; i++) {
        int c4_block_num = i / C4NUM;
        int c4_block_rem = i % C4NUM;
        int src_ic_offset = src_oc_offset + i;
        int dst_ic_offset = dst_oc_offset + c4_block_num * kernel_plane * C4NUM + c4_block_rem;
        (packed_weight_data + dst_ic_offset)[0] = (origin_weight_data + src_ic_offset)[0];
      }
    }
  }
}

void PackNHWCToNC4HW4Fp16(const void *src, void *dst, int batch, int plane, int channel) {
  int c4 = UP_DIV(channel, C4NUM);
  for (int b = 0; b < batch; b++) {
    int src_oc_offset = b * plane * channel;
    int dst_oc_offset = b * plane * c4 * C4NUM;
    for (int k = 0; k < plane; k++) {
      int src_kernel_offset = src_oc_offset + k * channel;
      int dst_kernel_offset = dst_oc_offset + k * C4NUM;
      for (int i = 0; i < channel; i++) {
        int c4_block_num = i / C4NUM;
        int c4_block_rem = i % C4NUM;
        int src_ic_offset = src_kernel_offset + i;
        int dst_ic_offset = dst_kernel_offset + c4_block_num * plane * C4NUM + c4_block_rem;
        ((float16_t *)dst + dst_ic_offset)[0] = ((float16_t *)src + src_ic_offset)[0];
      }
    }
  }
}

void PackNCHWToNC4HW4Fp16(const void *src, void *dst, int batch, int plane, int channel) {
  int c4 = UP_DIV(channel, C4NUM);
  for (int b = 0; b < batch; b++) {
    int src_offset = b * plane * channel;
    int dst_offset = b * plane * c4 * C4NUM;
    for (int c = 0; c < channel; c++) {
      int c4_block_num = c / C4NUM;
      int c4_block_rem = c % C4NUM;
      int src_c_offset = src_offset + c * plane;
      int dst_c_offset = dst_offset + c4_block_num * plane * C4NUM;
      for (int k = 0; k < plane; k++) {
        int src_kernel_offset = src_c_offset + k;
        int dst_kernel_offset = dst_c_offset + C4NUM * k + c4_block_rem;
        ((float16_t *)dst + dst_kernel_offset)[0] = ((float16_t *)src + src_kernel_offset)[0];
      }
    }
  }
}

void PackNHWCToNCHWFp16(const void *src, void *dst, int batches, int plane, int channel) {
  int hw16 = plane / C16NUM * C16NUM;
  int c8 = channel / C8NUM * C8NUM;
  int batch = plane * channel;
  for (int n = 0; n < batches; n++) {
    const float16_t *src_batch = (const float16_t *)src + n * batch;
    float16_t *dst_batch = (float16_t *)dst + n * batch;
    int hw = 0;
    for (; hw < hw16; hw += C16NUM) {
      int c = 0;
      for (; c < c8; c += C8NUM) {
        const float16_t *src_ptr = src_batch + hw * channel + c;
        float16_t *dst_ptr = dst_batch + c * plane + hw;
#ifdef ENABLE_ARM64
        size_t srcStride = channel * sizeof(float16_t);
        size_t dstStride = plane * sizeof(float16_t);
        asm volatile(
          "mov x10, %[src_ptr]\n"
          "mov x11, %[dst_ptr]\n"

          "ld1 {v0.8h}, [x10], %[srcStride]\n"
          "ld1 {v1.8h}, [x10], %[srcStride]\n"
          "ld1 {v2.8h}, [x10], %[srcStride]\n"
          "ld1 {v3.8h}, [x10], %[srcStride]\n"
          "ld1 {v4.8h}, [x10], %[srcStride]\n"
          "ld1 {v5.8h}, [x10], %[srcStride]\n"
          "ld1 {v6.8h}, [x10], %[srcStride]\n"
          "ld1 {v7.8h}, [x10], %[srcStride]\n"

          "zip1 v16.8h, v0.8h, v1.8h\n"
          "zip1 v17.8h, v2.8h, v3.8h\n"
          "zip1 v18.8h, v4.8h, v5.8h\n"
          "zip1 v19.8h, v6.8h, v7.8h\n"

          "ld1 {v8.8h}, [x10], %[srcStride]\n"
          "ld1 {v9.8h}, [x10], %[srcStride]\n"
          "ld1 {v10.8h}, [x10], %[srcStride]\n"
          "ld1 {v11.8h}, [x10], %[srcStride]\n"
          "ld1 {v12.8h}, [x10], %[srcStride]\n"
          "ld1 {v13.8h}, [x10], %[srcStride]\n"
          "ld1 {v14.8h}, [x10], %[srcStride]\n"
          "ld1 {v15.8h}, [x10], %[srcStride]\n"

          "trn1 v20.4s, v16.4s, v17.4s\n"
          "trn2 v21.4s, v16.4s, v17.4s\n"
          "trn1 v22.4s, v18.4s, v19.4s\n"
          "trn2 v23.4s, v18.4s, v19.4s\n"

          "trn1 v24.2d, v20.2d, v22.2d\n"
          "trn2 v25.2d, v20.2d, v22.2d\n"
          "trn1 v26.2d, v21.2d, v23.2d\n"
          "trn2 v27.2d, v21.2d, v23.2d\n"

          "zip1 v16.8h, v8.8h, v9.8h\n"
          "zip1 v17.8h, v10.8h, v11.8h\n"
          "zip1 v18.8h, v12.8h, v13.8h\n"
          "zip1 v19.8h, v14.8h, v15.8h\n"

          "trn1 v20.4s, v16.4s, v17.4s\n"
          "trn2 v21.4s, v16.4s, v17.4s\n"
          "trn1 v22.4s, v18.4s, v19.4s\n"
          "trn2 v23.4s, v18.4s, v19.4s\n"

          "trn1 v28.2d, v20.2d, v22.2d\n"
          "trn2 v29.2d, v20.2d, v22.2d\n"
          "trn1 v30.2d, v21.2d, v23.2d\n"
          "trn2 v31.2d, v21.2d, v23.2d\n"

          "add x10, x11, #16\n"
          "st1 {v24.8h}, [x11], %[dstStride]\n"
          "st1 {v28.8h}, [x10], %[dstStride]\n"
          "st1 {v26.8h}, [x11], %[dstStride]\n"
          "st1 {v30.8h}, [x10], %[dstStride]\n"
          "st1 {v25.8h}, [x11], %[dstStride]\n"
          "st1 {v29.8h}, [x10], %[dstStride]\n"
          "st1 {v27.8h}, [x11], %[dstStride]\n"
          "st1 {v31.8h}, [x10], %[dstStride]\n"

          "zip2 v16.8h, v0.8h, v1.8h\n"
          "zip2 v17.8h, v2.8h, v3.8h\n"
          "zip2 v18.8h, v4.8h, v5.8h\n"
          "zip2 v19.8h, v6.8h, v7.8h\n"

          "trn1 v20.4s, v16.4s, v17.4s\n"
          "trn2 v21.4s, v16.4s, v17.4s\n"
          "trn1 v22.4s, v18.4s, v19.4s\n"
          "trn2 v23.4s, v18.4s, v19.4s\n"

          "trn1 v24.2d, v20.2d, v22.2d\n"
          "trn2 v25.2d, v20.2d, v22.2d\n"
          "trn1 v26.2d, v21.2d, v23.2d\n"
          "trn2 v27.2d, v21.2d, v23.2d\n"

          "zip2 v16.8h, v8.8h, v9.8h\n"
          "zip2 v17.8h, v10.8h, v11.8h\n"
          "zip2 v18.8h, v12.8h, v13.8h\n"
          "zip2 v19.8h, v14.8h, v15.8h\n"

          "trn1 v20.4s, v16.4s, v17.4s\n"
          "trn2 v21.4s, v16.4s, v17.4s\n"
          "trn1 v22.4s, v18.4s, v19.4s\n"
          "trn2 v23.4s, v18.4s, v19.4s\n"

          "trn1 v28.2d, v20.2d, v22.2d\n"
          "trn2 v29.2d, v20.2d, v22.2d\n"
          "trn1 v30.2d, v21.2d, v23.2d\n"
          "trn2 v31.2d, v21.2d, v23.2d\n"

          "st1 {v24.8h}, [x11], %[dstStride]\n"
          "st1 {v28.8h}, [x10], %[dstStride]\n"
          "st1 {v26.8h}, [x11], %[dstStride]\n"
          "st1 {v30.8h}, [x10], %[dstStride]\n"
          "st1 {v25.8h}, [x11], %[dstStride]\n"
          "st1 {v29.8h}, [x10], %[dstStride]\n"
          "st1 {v27.8h}, [x11], %[dstStride]\n"
          "st1 {v31.8h}, [x10], %[dstStride]\n"
          :
          :
          [ dst_ptr ] "r"(dst_ptr), [ src_ptr ] "r"(src_ptr), [ srcStride ] "r"(srcStride), [ dstStride ] "r"(dstStride)
          : "x10", "x11", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", "v13", "v14",
            "v15", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29",
            "v30", "v31");
#else
        for (int tr = 0; tr < C16NUM; tr++) {
          for (int tc = 0; tc < C8NUM; tc++) {
            dst_ptr[tc * plane + tr] = src_ptr[tr * channel + tc];
          }
        }
#endif
      }
      for (; c < channel; c++) {
        const float16_t *src_ptr = src_batch + hw * channel + c;
        float16_t *dst_ptr = dst_batch + c * plane + hw;
        for (size_t i = 0; i < C16NUM; i++) {
          dst_ptr[i] = src_ptr[i * channel];
        }
      }
    }
    for (; hw < plane; hw++) {
      const float16_t *src_ptr = src_batch + hw * channel;
      float16_t *dst_ptr = dst_batch + hw;
      for (size_t i = 0; i < channel; i++) {
        dst_ptr[i * plane] = src_ptr[i];
      }
    }
  }
  return;
}

void PackNCHWToNHWCFp16(const void *src, void *dst, int batch, int plane, int channel) {
  return PackNHWCToNCHWFp16(src, dst, batch, channel, plane);
}

void PackNHWCToNHWC4Fp16(const void *src, void *dst, int batch, int plane, int channel) {
  int ic4 = UP_DIV(channel, C4NUM);
  int c4_channel = ic4 * C4NUM;
  int nhwc4_batch_unit_offset = ic4 * C4NUM * plane;
  int ic_remainder_ = channel % C4NUM;
  if (ic_remainder_ != 0) {
    int nhwc4_batch_offset = 0;
    for (int b = 0; b < batch; b++) {
      int batch_offset = b * channel * plane;
      for (int i = 0; i < plane; i++) {
        float16_t *dst_per_plane = (float16_t *)dst + nhwc4_batch_offset + i * c4_channel;
        memcpy(dst_per_plane, (float16_t *)src + batch_offset + i * channel, channel * sizeof(float16_t));
        for (int j = channel; j < c4_channel; ++j) {
          dst_per_plane[j] = 0;
        }
      }
      nhwc4_batch_offset += nhwc4_batch_unit_offset;
    }
  } else {
    size_t ori_input_size = batch * plane * channel * sizeof(float16_t);
    memcpy(dst, src, ori_input_size);
  }
}

void PackNHWCToNHWC8Fp16(const void *src, void *dst, int batch, int plane, int channel) {
  int ic8 = UP_DIV(channel, C8NUM);
  int c8_channel = ic8 * C8NUM;
  int nhwc8_batch_unit_offset = ic8 * C8NUM * plane;
  int ic_remainder_ = channel % C8NUM;
  if (ic_remainder_ != 0) {
    int nhwc8_batch_offset = 0;
    for (int b = 0; b < batch; b++) {
      int batch_offset = b * channel * plane;
      for (int i = 0; i < plane; i++) {
        float16_t *dst_per_plane = (float16_t *)dst + nhwc8_batch_offset + i * c8_channel;
        memcpy(dst_per_plane, (float16_t *)src + batch_offset + i * channel, channel * sizeof(float16_t));
        for (int j = channel; j < c8_channel; ++j) {
          dst_per_plane[j] = 0;
        }
      }
      nhwc8_batch_offset += nhwc8_batch_unit_offset;
    }
  } else {
    size_t ori_input_size = batch * plane * channel * sizeof(float16_t);
    memcpy(dst, src, ori_input_size);
  }
}

void PackNHWC4ToNHWCFp16(const void *src, void *dst, int batch, int plane, int channel) {
  int c4 = UP_DIV(channel, C4NUM);
  int ic_remainder_ = channel % C4NUM;
  if (ic_remainder_ != 0) {
    int nhwc_batch_unit_offset = channel * plane;
    for (int b = 0; b < batch; b++) {
      int batch_offset = b * c4 * C4NUM * plane;
      for (int i = 0; i < plane; i++) {
        memcpy((float16_t *)dst + b * nhwc_batch_unit_offset + i * channel,
               (float16_t *)src + batch_offset + i * c4 * C4NUM, channel * sizeof(float16_t));
      }
    }
  } else {
    size_t ori_input_size = batch * plane * channel * sizeof(float16_t);
    memcpy((float16_t *)dst, (float16_t *)src, ori_input_size);
  }
}

void PackNCHWToNHWC4Fp16(const void *src, void *dst, int batch, int plane, int channel) {
  int nhwc4_batch_offset = 0;
  int ic4 = UP_DIV(channel, C4NUM);
  int nhwc4_batch_unit_offset = ic4 * C4NUM * plane;

  for (int b = 0; b < batch; b++) {
    int batch_offset = b * channel * plane;
    for (int c = 0; c < channel; c++) {
      int src_c_offset = batch_offset + c * plane;
      int dst_c_offset = nhwc4_batch_offset + c;
      for (int i = 0; i < plane; i++) {
        int src_plane_offset = src_c_offset + i;
        int dst_plane_offset = dst_c_offset + i * ic4 * C4NUM;
        ((float16_t *)dst)[dst_plane_offset] = ((float16_t *)src)[src_plane_offset];
      }
    }
    nhwc4_batch_offset += nhwc4_batch_unit_offset;
  }
}

void PackNC4HW4ToNHWC4Fp16(const void *src, void *dst, int batch, int plane, int channel) {
  int c4 = UP_DIV(channel, C4NUM);
  for (int b = 0; b < batch; b++) {
    int src_offset = b * plane * c4 * C4NUM;
    int dst_offset = b * plane * channel;
    for (int c = 0; c < channel; c++) {
      int c4_block_num = c / C4NUM;
      int c4_block_res = c % C4NUM;
      int src_c_offset = src_offset + c4_block_num * plane * C4NUM + c4_block_res;
      int dst_c_offset = dst_offset + c4_block_num * C4NUM + c4_block_res;
      for (int k = 0; k < plane; k++) {
        int src_kernel_offset = src_c_offset + k * C4NUM;
        int dst_kernel_offset = dst_c_offset + k * c4 * C4NUM;
        ((float16_t *)dst + dst_kernel_offset)[0] = ((float16_t *)src + src_kernel_offset)[0];
      }
    }
  }
}

void PackNC4HW4ToNHWCFp16(const void *src, void *dst, int batch, int plane, int channel) {
  int c4 = UP_DIV(channel, C4NUM);
  for (int b = 0; b < batch; b++) {
    int src_offset = b * plane * c4 * C4NUM;
    int dst_offset = b * plane * channel;
    for (int c = 0; c < channel; c++) {
      int c4_block_num = c / C4NUM;
      int c4_block_res = c % C4NUM;
      int src_c_offset = src_offset + c4_block_num * plane * C4NUM + c4_block_res;
      int dst_c_offset = dst_offset + c;
      for (int k = 0; k < plane; k++) {
        int src_kernel_offset = src_c_offset + k * C4NUM;
        int dst_kernel_offset = dst_c_offset + k * channel;
        ((float16_t *)dst + dst_kernel_offset)[0] = ((float16_t *)src + src_kernel_offset)[0];
      }
    }
  }
}

void PackNC4HW4ToNCHWFp16(const void *src, void *dst, int batch, int plane, int channel) {
  int c4 = UP_DIV(channel, C4NUM);
  for (int b = 0; b < batch; b++) {
    int src_offset = b * plane * c4 * C4NUM;
    int dst_offset = b * plane * channel;
    for (int c = 0; c < channel; c++) {
      int c4_block_num = c / C4NUM;
      int c4_block_res = c % C4NUM;
      int src_c_offset = src_offset + c4_block_num * plane * C4NUM + c4_block_res;
      int dst_c_offset = dst_offset + c * plane;
      for (int k = 0; k < plane; k++) {
        int src_kernel_offset = src_c_offset + k * C4NUM;
        int dst_kernel_offset = dst_c_offset + k;
        ((float16_t *)dst + dst_kernel_offset)[0] = ((float16_t *)src + src_kernel_offset)[0];
      }
    }
  }
}

void PackNCHWFp32ToNC8HW8Fp16(float *src, float16_t *dst, int batch, int plane, int channel) {
  int c8 = UP_DIV(channel, C8NUM);
  for (int b = 0; b < batch; b++) {
    int src_offset = b * plane * channel;
    int dst_offset = b * plane * c8 * C8NUM;
    for (int c = 0; c < channel; c++) {
      int c8_block_num = c / C8NUM;
      int c8_block_rem = c % C8NUM;
      int src_c_offset = src_offset + c * plane;
      int dst_c_offset = dst_offset + c8_block_num * plane * C8NUM;
      for (int k = 0; k < plane; k++) {
        int src_kernel_offset = src_c_offset + k;
        int dst_kernel_offset = dst_c_offset + C8NUM * k + c8_block_rem;
        (dst + dst_kernel_offset)[0] = (float16_t)(src + src_kernel_offset)[0];
      }
    }
  }
}

void PackNHWCFp32ToNHWC8Fp16(float *src, float16_t *dst, int batch, int plane, int channel) {
  int c8_channel = UP_DIV(channel, C8NUM) * C8NUM;
  for (int b = 0; b < batch; b++) {
    float16_t *dst_batch = dst + b * plane * c8_channel;
    float *src_batch = src + b * plane * channel;
    for (int i = 0; i < plane; i++) {
      float16_t *dst_plane = dst_batch + i * c8_channel;
      float *src_plane = src_batch + i * channel;
      for (int c = 0; c < channel; c++) {
        dst_plane[c] = (float16_t)(src_plane[c]);
      }
    }
  }
}

void PackNHWCFp32ToC8HWN8Fp16(float *src, float16_t *dst, int batch, int plane, int channel) {
  for (int n = 0; n < batch; n++) {
    for (int hw = 0; hw < plane; hw++) {
      for (int c = 0; c < channel; c++) {
        int c8div = c / C8NUM;
        int c8mod = c % C8NUM;
        int src_index = n * plane * channel + hw * channel + c;
        int dst_index = c8div * batch * plane * C8NUM + hw * batch * C8NUM + n * C8NUM + c8mod;
        dst[dst_index] = (float16_t)(src[src_index]);
      }
    }
  }
  return;
}

void PackNHWC8Fp16ToNHWCFp32(float16_t *src, float *dst, int batch, int plane, int channel) {
  int c8_channel = UP_DIV(channel, C8NUM) * C8NUM;
  for (int b = 0; b < batch; b++) {
    float16_t *src_batch = src + b * plane * c8_channel;
    float *dst_batch = dst + b * plane * channel;
    for (int i = 0; i < plane; i++) {
      float16_t *src_plane = src_batch + i * c8_channel;
      float *dst_plane = dst_batch + i * channel;
      for (int c = 0; c < channel; c++) {
        dst_plane[c] = (float16_t)(src_plane[c]);
      }
    }
  }
}

void PackNHWC8ToNHWCFp16(float16_t *src, float16_t *dst, int batch, int plane, int channel) {
  int c8_channel = UP_DIV(channel, C8NUM) * C8NUM;
  for (int b = 0; b < batch; b++) {
    float16_t *src_batch = src + b * plane * c8_channel;
    float16_t *dst_batch = dst + b * plane * channel;
    for (int i = 0; i < plane; i++) {
      float16_t *src_plane = src_batch + i * c8_channel;
      float16_t *dst_plane = dst_batch + i * channel;
      memcpy(dst_plane, src_plane, channel * sizeof(float16_t));
    }
  }
}
