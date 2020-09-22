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

#include "nnacl/fp32/conv.h"
#include <string.h>
#include "nnacl/fp32/common_func.h"
#include "nnacl/winograd_transform.h"
#include "nnacl/fp32/matmul.h"

void SWBorderPixel(float *dst, const float *src, const float *weight, const float *bias, int height, int width,
                   int in_kh_step, int in_kw_step, int kernel_h, int kernel_w, int ic4, bool is_relu, bool is_relu6) {
  for (int c = 0; c < C4NUM; c++) {
    dst[c] = 0;
  }
  const float *weight_oc = weight;
  for (int oc = 0; oc < C4NUM; ++oc) {
    const float *weight_kh = weight_oc;
    const float *src_kh = src;
    for (int kh = 0; kh < height; kh++) {
      const float *src_kw = src_kh;
      const float *weight_kw = weight_kh;
      for (int kw = 0; kw < width; kw++) {
        const float *src_ic4 = src_kw;
        const float *weight_ic4 = weight_kw;
        for (int ic = 0; ic < ic4; ++ic) {
          for (int c = 0; c < C4NUM; c++) {
            dst[oc] += src_ic4[c] * weight_ic4[c];
          }
          src_ic4 += C4NUM;
          weight_ic4 += C4NUM;
        }  // ic4 loop
        src_kw += in_kw_step;
        weight_kw += ic4 * C4NUM;
      }  // kernel_w loop
      src_kh += in_kh_step;
      weight_kh += kernel_w * ic4 * C4NUM;
    }  // kernel_h loop
    dst[oc] += bias[oc];
    dst[oc] = (is_relu) ? (MSMAX(0, dst[oc])) : (dst[oc]);
    dst[oc] = (is_relu6) ? (MSMIN(6, MSMAX(0, dst[oc]))) : (dst[oc]);
    weight_oc += kernel_h * kernel_w * ic4 * C4NUM;
  }  // oc loop
}

void SWBorder(float *dst, const float *src, const float *weight, const float *bias, int top, int bottom, int left,
              int right, const ConvParameter *conv_param, const SlidingWindowParam *sliding) {
  int ic4 = sliding->ic4_channel_ / C4NUM;
  bool relu = conv_param->act_type_ == ActType_Relu;
  bool relu6 = conv_param->act_type_ == ActType_Relu6;
  float *dst_h = dst + top * sliding->out_h_step_;
  for (int oh = top; oh < bottom; oh++) {
    int ih = oh * conv_param->stride_h_ - conv_param->pad_u_;
    int start_kh = MSMAX(0, UP_DIV(-ih, conv_param->dilation_h_));
    int end_kh = MSMIN(conv_param->kernel_h_, UP_DIV(conv_param->input_h_ - ih, conv_param->dilation_h_));
    const float *src_h = src + ih * sliding->in_h_step_;

    float *dst_kernel = dst_h + left * sliding->block_channel_;
    for (int ow = left; ow < right; ow++) {
      int iw = ow * conv_param->stride_w_ - conv_param->pad_l_;
      int start_kw = MSMAX(0, UP_DIV(-iw, conv_param->dilation_w_));
      int end_kw = MSMIN(conv_param->kernel_w_, UP_DIV(conv_param->input_w_ - iw, conv_param->dilation_w_));
      const float *src_w = src_h + iw * sliding->ic4_channel_;

      const float *src_kernel = src_w + start_kh * sliding->in_kh_step_ + start_kw * sliding->in_kw_step_;
      const float *weight_kernel = weight + (start_kh * conv_param->kernel_w_ + start_kw) * sliding->ic4_channel_;

      SWBorderPixel(dst_kernel, src_kernel, weight_kernel, bias, end_kh - start_kh, end_kw - start_kw,
                    sliding->in_kh_step_, sliding->in_kw_step_, conv_param->kernel_h_, conv_param->kernel_w_, ic4, relu,
                    relu6);

      dst_kernel += sliding->block_channel_;
    }  // width loop
    dst_h += sliding->out_h_step_;
  }  // height loop
}

#ifndef ENABLE_ARM64
void SWCenter(float *dst, const float *src, const float *weight, const float *bias, int height, int width, int kernel_h,
              int kernel_w, int out_h_step, int block_channel, int ic4, int in_sh_step, int in_sw_step, int in_kh_step,
              int in_kw_step, bool is_relu, bool is_relu6) {
  float *dst_h = dst;
  const float *src_h = src;
  for (int oh = 0; oh < height; oh++) {
    float *dst_w = dst_h;
    const float *src_w = src_h;
    for (int ow = 0; ow < width; ow++) {
      const float *weight_oc = weight;
      for (int c = 0; c < C4NUM; c++) {
        dst_w[c] = 0;
      }

      for (int oc = 0; oc < C4NUM; oc++) {
        const float *weight_kh = weight_oc;
        const float *src_kh = src_w;
        for (int kh = 0; kh < kernel_h; kh++) {
          const float *src_kw = src_kh;
          const float *weight_kw = weight_kh;
          for (int kw = 0; kw < kernel_w; kw++) {
            const float *src_ic4 = src_kw;
            const float *weight_ic4 = weight_kw;
            for (int ic = 0; ic < ic4; ++ic) {
              for (int c = 0; c < C4NUM; c++) {
                dst_w[oc] += src_ic4[c] * weight_ic4[c];
              }

              src_ic4 += C4NUM;
              weight_ic4 += C4NUM;
            }  // ic4 loop
            src_kw += in_kw_step;
            weight_kw += ic4 * C4NUM;
          }  // kernel_w loop
          src_kh += in_kh_step;
          weight_kh += kernel_w * ic4 * C4NUM;
        }  // kernel_h loop
           // add biad relu

        dst_w[oc] += bias[oc];
        dst_w[oc] = (is_relu) ? (MSMAX(0, dst_w[oc])) : (dst_w[oc]);
        dst_w[oc] = (is_relu6) ? (MSMIN(6, MSMAX(0, dst_w[oc]))) : (dst_w[oc]);
        weight_oc += kernel_h * kernel_w * ic4 * C4NUM;
      }  // oc block

      dst_w += block_channel;
      src_w += in_sw_step;
    }  // dst_width loop
    dst_h += out_h_step;
    src_h += in_sh_step;
  }  // dst_height loop
}
#endif

// fp32 sliding window
void ConvSWFp32(const float *input_data, const float *packed_weight, const float *bias_data, float *tmp_out_block,
                float *output_data, int task_id, ConvParameter *conv_param, SlidingWindowParam *slidingWindow_param) {
  int ic4 = slidingWindow_param->ic4_channel_ / C4NUM;
  int oc4_res = conv_param->output_channel_ % C4NUM;
  bool relu = conv_param->act_type_ == ActType_Relu;
  bool relu6 = conv_param->act_type_ == ActType_Relu6;
  const float *src = input_data;
  float *dst = NULL;
  if (oc4_res == 0) {
    dst = output_data;
  } else {
    dst = tmp_out_block;
  }

  for (int b = 0; b < conv_param->output_batch_; b++) {
    for (int oc = task_id; oc < slidingWindow_param->c_block_; oc += conv_param->thread_num_) {
      const float *src_data = src;
      float *dst_data = dst + oc * C4NUM;
      const float *weight = packed_weight + oc * slidingWindow_param->kernel_step_;
      const float *bias = bias_data + oc * C4NUM;
      SWBorder(dst_data, src_data, weight, bias, 0, slidingWindow_param->top_, 0, conv_param->output_w_, conv_param,
               slidingWindow_param);
      SWBorder(dst_data, src_data, weight, bias, slidingWindow_param->bottom_, conv_param->output_h_, 0,
               conv_param->output_w_, conv_param, slidingWindow_param);
      SWBorder(dst_data, src_data, weight, bias, slidingWindow_param->top_, slidingWindow_param->bottom_, 0,
               slidingWindow_param->left_, conv_param, slidingWindow_param);
      SWBorder(dst_data, src_data, weight, bias, slidingWindow_param->top_, slidingWindow_param->bottom_,
               slidingWindow_param->right_, conv_param->output_w_, conv_param, slidingWindow_param);

      if (slidingWindow_param->right_ > slidingWindow_param->left_ &&
          slidingWindow_param->bottom_ > slidingWindow_param->top_) {
        int in_h_start = slidingWindow_param->top_ * conv_param->stride_h_ - conv_param->pad_u_;
        int in_w_start = slidingWindow_param->left_ * conv_param->stride_w_ - conv_param->pad_l_;
        const float *in_t =
          src_data + in_h_start * slidingWindow_param->in_h_step_ + in_w_start * slidingWindow_param->ic4_channel_;
        float *out_t = dst_data + slidingWindow_param->top_ * slidingWindow_param->out_h_step_ +
                       slidingWindow_param->left_ * slidingWindow_param->block_channel_;
#ifdef ENABLE_ARM64
        ConvSwFp32Center(
          out_t, in_t, weight, bias, slidingWindow_param->bottom_ - slidingWindow_param->top_,
          slidingWindow_param->right_ - slidingWindow_param->left_, conv_param->kernel_h_, conv_param->kernel_w_,
          slidingWindow_param->out_h_step_ * sizeof(float), slidingWindow_param->block_channel_ * sizeof(float), ic4,
          slidingWindow_param->in_sh_step_ * sizeof(float), slidingWindow_param->in_sw_step_ * sizeof(float),
          slidingWindow_param->in_kh_step_ * sizeof(float), slidingWindow_param->in_kw_step_ * sizeof(float), relu,
          relu6);
#else
        SWCenter(out_t, in_t, weight, bias, slidingWindow_param->bottom_ - slidingWindow_param->top_,
                 slidingWindow_param->right_ - slidingWindow_param->left_, conv_param->kernel_h_, conv_param->kernel_w_,
                 slidingWindow_param->out_h_step_, slidingWindow_param->block_channel_, ic4,
                 slidingWindow_param->in_sh_step_, slidingWindow_param->in_sw_step_, slidingWindow_param->in_kh_step_,
                 slidingWindow_param->in_kw_step_, relu, relu6);
#endif
      }
    }  // output C4 loop
    src += slidingWindow_param->in_step_;
    dst += slidingWindow_param->out_step_;
  }  // batch loop
}

// fp32 conv common
void ConvFp32(float *input_data, float *packed_input, float *packed_weight, const float *bias_data,
              float *tmp_out_block, float *output_data, int task_id, ConvParameter *conv_param,
              GEMM_FUNC_FP32 gemm_func) {
  int kernel_h = conv_param->kernel_h_;
  int kernel_w = conv_param->kernel_w_;
  int in_batch = conv_param->input_batch_;
  int in_channel = conv_param->input_channel_;
  int in_h = conv_param->input_h_;
  int in_w = conv_param->input_w_;
  int out_h = conv_param->output_h_;
  int out_w = conv_param->output_w_;
  int out_channel = conv_param->output_channel_;
  int thread_count = conv_param->thread_num_;
  int output_count = out_h * out_w;
  int output_tile_count = UP_DIV(output_count, TILE_NUM);
  int ic4 = UP_DIV(in_channel, C4NUM);
  int kernel_plane = kernel_h * kernel_w;
  int unit_size = kernel_plane * ic4 * C4NUM;
  int packed_input_size = output_tile_count * TILE_NUM * unit_size;
  bool relu = conv_param->act_type_ == ActType_Relu;
  bool relu6 = conv_param->act_type_ == ActType_Relu6;

  // we accumulate 4 channels per time for input blocks
  int conv_depth = kernel_h * kernel_w;
  // bytes from one output's i-th channel to the next output's i-th channel
  // we write 32 bytes per st1 instruction, after which the pointer in register will step 32B forward
  size_t output_offset = out_channel * sizeof(float);

  for (int b = 0; b < in_batch; b++) {
    int in_batch_offset = b * ic4 * C4NUM * in_h * in_w;
    int out_batch_offset = b * out_channel * out_h * out_w;
    int gemm_in_batch_offset = b * packed_input_size;
    for (int thread_id = task_id; thread_id < output_tile_count; thread_id += thread_count) {
      int start_index = thread_id * TILE_NUM;
      int real_cal_num = (output_count - start_index) < TILE_NUM ? (output_count - start_index) : TILE_NUM;
      float *gemm_input = packed_input + thread_id * unit_size * TILE_NUM + gemm_in_batch_offset;
      Im2ColPackUnitFp32(input_data + in_batch_offset, conv_param, gemm_input, real_cal_num, start_index);

      int out_offset = thread_id * TILE_NUM * out_channel + out_batch_offset;
      if (real_cal_num == TILE_NUM) {
        float *gemm_output = output_data + out_offset;
        gemm_func(gemm_output, gemm_input, packed_weight, bias_data, conv_depth, ic4, out_channel, output_offset, 0, 0,
                  relu, relu6);
      } else {
        // res part
        float *tmp_out_ptr = tmp_out_block + task_id * TILE_NUM * out_channel;
        gemm_func(tmp_out_ptr, gemm_input, packed_weight, bias_data, conv_depth, ic4, out_channel, output_offset, 0, 0,
                  relu, relu6);
        memcpy(output_data + out_offset, tmp_out_ptr, real_cal_num * out_channel * sizeof(float));
      }
    }
  }
}

// fp32 conv winograd
void ConvWinogardFp32(float *input_data, float *trans_weight, const float *bias_data, float *output_data,
                      TmpBufferAddress *buffer_list, int task_id, ConvParameter *conv_param, InputTransFunc in_func,
                      OutputTransFunc out_func) {
  int thread_num = conv_param->thread_num_;
  int input_unit = conv_param->input_unit_;
  int in_batch = conv_param->input_batch_;
  int in_channel = conv_param->input_channel_;
  int ic4 = UP_DIV(in_channel, C4NUM);
  int out_unit = conv_param->output_unit_;
  int out_w_block = UP_DIV(conv_param->output_w_, out_unit);
  int out_h_block = UP_DIV(conv_param->output_h_, out_unit);
  int output_count = out_w_block * out_h_block;
#ifdef ENABLE_ARM32
  const int tile_num = 4;
#else
  const int tile_num = 12;
#endif
  int output_tile_count = UP_DIV(output_count, tile_num);
  int out_channel = conv_param->output_channel_;
  int oc8 = UP_DIV(out_channel, C8NUM);
  int input_unit_square = input_unit * input_unit;

  float *trans_input = buffer_list[0];
  float *gemm_out = buffer_list[1];
  float *tmp_data = buffer_list[3];
  float *col_buffer = buffer_list[4];
  int trans_input_offset = tile_num * input_unit_square * ic4 * C4NUM;
  int gemm_out_offset = tile_num * input_unit_square * oc8 * C8NUM;
  int tmp_data_offset = input_unit_square * C4NUM;
  int col_buffer_offset = tile_num * ic4 * C4NUM;
  // step 1 : filter transform (pre-processed offline)
  // step 2 : input transform (online)
  for (int b = 0; b < in_batch; b++) {
    int in_batch_offset = b * ic4 * C4NUM * conv_param->input_h_ * conv_param->input_w_;
    int out_batch_offset = b * out_channel * conv_param->output_w_ * conv_param->output_h_;
    for (int thread_id = task_id; thread_id < output_tile_count; thread_id += thread_num) {
      int out_tile_index = thread_id * tile_num;
      int cal_num = output_count - thread_id * tile_num;
      cal_num = cal_num > tile_num ? tile_num : cal_num;
      WinogradInputTransform(input_data + in_batch_offset, trans_input + task_id * trans_input_offset,
                             tmp_data + task_id * tmp_data_offset, cal_num, out_tile_index, out_w_block, conv_param,
                             in_func);
      // step 3 : gemm
      float *src_ptr = trans_input + task_id * trans_input_offset;
      float *dst_ptr = gemm_out + task_id * gemm_out_offset;
      float *tmp_col_ptr = col_buffer + task_id * col_buffer_offset;
      for (int i = 0; i < input_unit_square; ++i) {
#ifdef ENABLE_ARM32
        RowMajor2Col4Major(src_ptr + i * C4NUM * ic4 * C4NUM, tmp_col_ptr, C4NUM, ic4 * C4NUM);
#else
        RowMajor2Col12Major(src_ptr + i * C12NUM * ic4 * C4NUM, tmp_col_ptr, C12NUM, ic4 * C4NUM);
#endif
        MatMulOpt(tmp_col_ptr, trans_weight + i * ic4 * C4NUM * oc8 * C8NUM, dst_ptr + i * C8NUM, NULL, 0, ic4 * C4NUM,
                  cal_num, oc8 * C8NUM, input_unit_square, 2);
      }

      // step 4 : output transform
      float *output_ptr = output_data + out_batch_offset;
      WinogradOutputTransform(dst_ptr, output_ptr, bias_data, cal_num, out_tile_index, out_w_block, conv_param,
                              out_func);
    }
  }
}

void UnPackWinogradOutput(const float *src, float *dst, int batch, int height, int width, int channel,
                          int output_unit) {
  int out_h_block_num = UP_DIV(height, output_unit);
  int out_w_block_num = UP_DIV(width, output_unit);
  int c4 = UP_DIV(channel, C4NUM);
  int c4_block = C4NUM * out_h_block_num * output_unit * out_w_block_num * output_unit;
  for (int b = 0; b < batch; b++) {
    int src_batch_offset = b * c4 * c4_block;
    int dst_batch_offset = b * height * width * channel;
    for (int h = 0; h < height; h++) {
      int src_h_offset = src_batch_offset + C4NUM * (h * out_w_block_num * output_unit);
      int dst_h_offset = dst_batch_offset + h * width * channel;
      for (int w = 0; w < width; w++) {
        int src_w_offset = src_h_offset + w * C4NUM;
        int dst_w_offset = dst_h_offset + w * channel;
        for (int c = 0; c < c4 - 1; c++) {
          int src_c4_offset = src_w_offset + c * c4_block;
          int dst_c4_offset = dst_w_offset + c * C4NUM;
#ifdef ENABLE_NEON
          vst1q_f32(dst + dst_c4_offset, vld1q_f32(src + src_c4_offset));
#else
          for (int i = 0; i < C4NUM; ++i) {
            dst[dst_c4_offset + i] = src[src_c4_offset + i];
          }
#endif
        }
        int c_res = channel - (c4 - 1) * C4NUM;
        int src_c_res_offset = (c4 - 1) * c4_block;
        int dst_c_res_offset = (c4 - 1) * C4NUM;
        for (int c = 0; c < c_res; c++) {
          int src_c4_res_offset = src_w_offset + src_c_res_offset + c;
          int dst_c4_res_offset = dst_w_offset + dst_c_res_offset + c;
          dst[dst_c4_res_offset] = src[src_c4_res_offset];
        }
      }
    }
  }
}

void UnPackWinogradReluOutput(const float *src, float *dst, int batch, int height, int width, int channel,
                              int output_unit) {
  int out_h_block_num = UP_DIV(height, output_unit);
  int out_w_block_num = UP_DIV(width, output_unit);
  int c4 = UP_DIV(channel, C4NUM);
  int c4_block = C4NUM * out_h_block_num * output_unit * out_w_block_num * output_unit;
  for (int b = 0; b < batch; b++) {
    int src_batch_offset = b * c4 * c4_block;
    int dst_batch_offset = b * height * width * channel;
    for (int h = 0; h < height; h++) {
      int src_h_offset = src_batch_offset + C4NUM * (h * out_w_block_num * output_unit);
      int dst_h_offset = dst_batch_offset + h * width * channel;
      for (int w = 0; w < width; w++) {
        int src_w_offset = src_h_offset + w * C4NUM;
        int dst_w_offset = dst_h_offset + w * channel;
        for (int c = 0; c < c4 - 1; c++) {
          int src_c4_offset = src_w_offset + c * c4_block;
          int dst_c4_offset = dst_w_offset + c * C4NUM;
#ifdef ENABLE_NEON
          float32x4_t input_ptr = vld1q_f32(src + src_c4_offset);
          float32x4_t zero = vdupq_n_f32(0);
          input_ptr = vmaxq_f32(zero, input_ptr);
          vst1q_f32(dst + dst_c4_offset, input_ptr);
#else
          for (int i = 0; i < C4NUM; ++i) {
            float input_data = src[src_c4_offset + i];
            input_data = input_data < 0 ? 0 : input_data;
            dst[dst_c4_offset + i] = input_data;
          }
#endif
        }
        int c_res = channel - (c4 - 1) * C4NUM;
        int src_c_res_offset = (c4 - 1) * c4_block;
        int dst_c_res_offset = (c4 - 1) * C4NUM;
        for (int c = 0; c < c_res; c++) {
          int src_c4_res_offset = src_w_offset + src_c_res_offset + c;
          int dst_c4_res_offset = dst_w_offset + dst_c_res_offset + c;
          float input_data = src[src_c4_res_offset];
          input_data = input_data < 0 ? 0 : input_data;
          dst[dst_c4_res_offset] = input_data;
        }
      }
    }
  }
}

void UnPackWinogradRelu6Output(const float *src, float *dst, int batch, int height, int width, int channel,
                               int output_unit) {
  int out_h_block_num = UP_DIV(height, output_unit);
  int out_w_block_num = UP_DIV(width, output_unit);
  int c4 = UP_DIV(channel, C4NUM);
  int c4_block = C4NUM * out_h_block_num * output_unit * out_w_block_num * output_unit;
  for (int b = 0; b < batch; b++) {
    int src_batch_offset = b * c4 * c4_block;
    int dst_batch_offset = b * height * width * channel;
    for (int h = 0; h < height; h++) {
      int src_h_offset = src_batch_offset + C4NUM * (h * out_w_block_num * output_unit);
      int dst_h_offset = dst_batch_offset + h * width * channel;
      for (int w = 0; w < width; w++) {
        int src_w_offset = src_h_offset + w * C4NUM;
        int dst_w_offset = dst_h_offset + w * channel;
        for (int c = 0; c < c4 - 1; c++) {
          int src_c4_offset = src_w_offset + c * c4_block;
          int dst_c4_offset = dst_w_offset + c * C4NUM;
#ifdef ENABLE_NEON
          float32x4_t input_ptr = vld1q_f32(src + src_c4_offset);
          float32x4_t zero = vdupq_n_f32(0);
          float32x4_t six = vdupq_n_f32(6);
          input_ptr = vmaxq_f32(zero, input_ptr);
          input_ptr = vminq_f32(six, input_ptr);
          vst1q_f32(dst + dst_c4_offset, input_ptr);
#else
          for (int i = 0; i < C4NUM; ++i) {
            float input_data = src[src_c4_offset + i];
            input_data = input_data < 0 ? 0 : input_data;
            input_data = input_data > 6 ? 6 : input_data;
            dst[dst_c4_offset + i] = input_data;
          }
#endif
        }
        int c_res = channel - (c4 - 1) * C4NUM;
        int src_c_res_offset = (c4 - 1) * c4_block;
        int dst_c_res_offset = (c4 - 1) * C4NUM;
        for (int c = 0; c < c_res; c++) {
          int src_c4_res_offset = src_w_offset + src_c_res_offset + c;
          int dst_c4_res_offset = dst_w_offset + dst_c_res_offset + c;
          float input_data = src[src_c4_res_offset];
          input_data = input_data < 0 ? 0 : input_data;
          input_data = input_data > 6 ? 6 : input_data;
          dst[dst_c4_res_offset] = input_data;
        }
      }
    }
  }
}

// fp32 conv3x3
void Conv3x3Fp32(float *input_data, float *transed_weight, const float *bias_data, TmpBufferAddress *buffer_list,
                 int task_id, ConvParameter *conv_param, GEMM_FUNC_FP32 gemm_func) {
  int thread_count = conv_param->thread_num_;
  int ic4 = UP_DIV(conv_param->input_channel_, C4NUM);
  int output_channel = conv_param->output_channel_;
  int oc4 = UP_DIV(output_channel, C4NUM);
  int oc8 = UP_DIV(output_channel, C8NUM);
  int out_w_block = UP_DIV(conv_param->output_w_, OUPUT_UNIT);
  int out_h_block = UP_DIV(conv_param->output_h_, OUPUT_UNIT);
  int output_count = out_w_block * out_h_block;
#ifdef ENABLE_ARM32
  const int tile_num = 4;
#else
  const int tile_num = 12;
#endif
  int output_tile_count = UP_DIV(output_count, tile_num);
  const int input_unit_square = 4 * 4;

  float *tile_buffer = buffer_list[0];
  float *block_unit_buffer = buffer_list[1];
  float *tmp_dst_buffer = buffer_list[2];
  float *nc4hw4_out = buffer_list[3];
  float *col_buffer = buffer_list[4];
  int tile_buffer_offset = tile_num * input_unit_square * ic4 * C4NUM;
  int block_unit_buffer_offset = input_unit_square * C4NUM;
  int tmp_dst_buffer_offset = tile_num * input_unit_square * oc8 * C8NUM;
  int col_buffer_offset = tile_num * ic4 * C4NUM;

  int input_batch = conv_param->input_batch_;
  for (int batch = 0; batch < input_batch; batch++) {
    int in_batch_offset = batch * ic4 * C4NUM * conv_param->input_h_ * conv_param->input_w_;
    int nc4hw4_buffer_offset = batch * oc4 * C4NUM * conv_param->output_h_ * conv_param->output_w_;

    for (int thread_id = task_id; thread_id < output_tile_count; thread_id += thread_count) {
      int start_index = thread_id * tile_num;
      int real_cal_num = (output_count - start_index) < tile_num ? (output_count - start_index) : tile_num;
      Conv3x3Fp32InputTransform(input_data + in_batch_offset, tile_buffer + task_id * tile_buffer_offset,
                                block_unit_buffer + task_id * block_unit_buffer_offset, start_index, real_cal_num,
                                out_w_block, conv_param);

      float *src_ptr = tile_buffer + task_id * tile_buffer_offset;
      float *tmp_col_ptr = col_buffer + task_id * col_buffer_offset;
      float *dst_ptr = tmp_dst_buffer + task_id * tmp_dst_buffer_offset;
      for (int i = 0; i < input_unit_square; ++i) {
#ifdef ENABLE_ARM32
        RowMajor2Col4Major(src_ptr + i * C4NUM * ic4 * C4NUM, tmp_col_ptr, C4NUM, ic4 * C4NUM);
#else
        RowMajor2Col12Major(src_ptr + i * C12NUM * ic4 * C4NUM, tmp_col_ptr, C12NUM, ic4 * C4NUM);
#endif
        MatMulOpt(tmp_col_ptr, transed_weight + i * ic4 * C4NUM * oc8 * C8NUM, dst_ptr + i * C8NUM, NULL, 0,
                  ic4 * C4NUM, real_cal_num, oc8 * C8NUM, input_unit_square, 2);
      }
      Conv3x3Fp32OutputTransform(dst_ptr, nc4hw4_out + nc4hw4_buffer_offset, bias_data, start_index, real_cal_num,
                                 out_w_block, conv_param);
    }
  }
}
