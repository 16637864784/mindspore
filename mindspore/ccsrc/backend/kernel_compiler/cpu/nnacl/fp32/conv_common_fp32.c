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

#include "nnacl/fp32/conv_common_fp32.h"
#include <string.h>
#ifdef ENABLE_AVX
#include <x86intrin.h>
#endif
#include "nnacl/fp32/matmul_fp32.h"

// fp32 conv common
void ConvFp32(const float *input_data, float *packed_input, const float *packed_weight, const float *bias_data,
              float *col_major_input, float *output_data, int task_id, const ConvParameter *conv_param) {
  int out_channel = conv_param->output_channel_;
  int deep = conv_param->kernel_h_ * conv_param->kernel_w_ * conv_param->input_channel_;
  int output_count = conv_param->output_h_ * conv_param->output_w_;
#ifdef ENABLE_AVX
  const int cal_num = C6NUM;
#elif defined(ENABLE_SSE)
  const int cal_num = C4NUM;
#else
  const int cal_num = C12NUM;
#endif
  int output_tile_count = UP_DIV(output_count, cal_num);

  for (int b = 0; b < conv_param->input_batch_; b++) {
    int in_batch_offset = b * conv_param->input_channel_ * conv_param->input_h_ * conv_param->input_w_;
    int out_batch_offset = b * out_channel * output_count;
    for (int thread_id = task_id; thread_id < output_tile_count; thread_id += conv_param->thread_num_) {
      int start_index = thread_id * cal_num;
      int real_cal_num = (output_count - start_index) < cal_num ? (output_count - start_index) : cal_num;
      if (real_cal_num <= 0) {
        return;
      }
      float *gemm_input = packed_input + task_id * deep * cal_num;
      float *col_major_gemm_input = col_major_input + task_id * deep * cal_num;
      size_t packed_input_size = deep * cal_num * sizeof(float);
      memset(gemm_input, 0, packed_input_size);
      memset(col_major_gemm_input, 0, packed_input_size);
      Im2ColPackUnitFp32(input_data + in_batch_offset, conv_param, gemm_input, real_cal_num, start_index);

      int out_offset = thread_id * cal_num * out_channel + out_batch_offset;
      float *gemm_output = output_data + out_offset;
#ifdef ENABLE_AVX
      RowMajor2Col6Major(gemm_input, col_major_gemm_input, cal_num, deep);
#elif defined(ENABLE_SSE)
      RowMajor2Col4Major(gemm_input, col_major_gemm_input, cal_num, deep);
#else
      RowMajor2Col12Major(gemm_input, col_major_gemm_input, cal_num, deep);
#endif
      MatMulOpt(col_major_gemm_input, packed_weight, gemm_output, bias_data, conv_param->act_type_, deep, real_cal_num,
                out_channel, out_channel, OutType_Nhwc);
    }
  }
}

#ifdef ENABLE_AVX

void SWBorder(float *dst, const float *src, const float *weight, const float *bias, int top, int bottom, int left,
              int right, const ConvParameter *conv_param, const SlidingWindowParam *sw_param, const SWConvKernel kernel,
              int act_type, int ow_bock, int oc_block) {
  for (int oh = top; oh < bottom; oh++) {  // now h is only loop one time
    int ih = oh * conv_param->stride_h_ - conv_param->pad_u_;
    int start_kh = MSMAX(0, UP_DIV(-ih, conv_param->dilation_h_));
    int end_kh = MSMIN(conv_param->kernel_h_, UP_DIV(conv_param->input_h_ - ih, conv_param->dilation_h_));
    const float *src_h = src + ih * sw_param->in_h_step_;
    float *dst_kernel = dst + left * sw_param->block_channel_;
    for (int ow = left; ow < right; ow += ow_bock) {
      int iw = ow * conv_param->stride_w_ - conv_param->pad_l_;
      int start_kw = MSMAX(0, UP_DIV(-iw, conv_param->dilation_w_));
      int end_kw = MSMIN(conv_param->kernel_w_, UP_DIV(conv_param->input_w_ - iw, conv_param->dilation_w_));
      const float *src_w = src_h + iw * sw_param->ic_align_;
      const float *src_kernel = src_w + start_kh * sw_param->in_kh_step_ + start_kw * sw_param->in_kw_step_;
      const float *weight_kernel =
        weight + (start_kh * conv_param->kernel_w_ + start_kw) * sw_param->ic_align_ * C8NUM * oc_block;
      kernel(dst_kernel, src_kernel, weight_kernel, bias, end_kh - start_kh, end_kw - start_kw, act_type, ow_bock,
             oc_block, sw_param->block_channel_, sw_param->ic_align_, sw_param->in_kw_step_, sw_param->in_kh_step_,
             sw_param->in_sw_step_,
             (conv_param->kernel_w_ - end_kw + start_kw) * C8NUM * oc_block * sw_param->ic_align_);
      dst_kernel += ow_bock * sw_param->block_channel_;
    }  // width loop
    dst += sw_param->out_h_step_;
  }  // height loop
}

// fp32 sliding window
void ConvSWFp32(const float *input_data, const float *packed_weight, const float *bias_data, float *output_data,
                int task_id, ConvParameter *conv_param, SlidingWindowParam *sw_param) {
  int oc_tile_ = C8NUM;  // oc in algin to C8NUM in x86_64_avx
  int act_type = 0;
  if (conv_param->act_type_ == ActType_Relu6) {
    act_type += 1;
  }
  if (conv_param->act_type_ == ActType_Relu || conv_param->act_type_ == ActType_Relu6) {
    act_type += 2;
  }
  int kernel_h = conv_param->kernel_h_;
  int kernel_w = conv_param->kernel_w_;
  int oc_algin = sw_param->block_channel_;
  int ic_algin = sw_param->ic_align_;
  int in_sw_step = sw_param->in_sw_step_;
  int in_kw_step = sw_param->in_kw_step_;
  int in_kh_step = sw_param->in_kh_step_;
  const int ow_block_num[4] = {12, 6, 4, 3};
#ifdef ENABLE_DEBUG
  const SWConvKernel kernel[4][2] = {{SWConvWxKKernel, SWConvWxKKernel},
                                     {SWConvWxKKernel, SWConvWxKKernel},
                                     {SWConvWxKKernel, SWConvWxKKernel},
                                     {SWConvWxKKernel, SWConvWxKKernel}};
#else
  const SWConvKernel kernel[4][2] = {{SWConv1x8Kernel, SWConv12x8Kernel},
                                     {SWConv1x16Kernel, SWConv6x16Kernel},
                                     {SWConv1x24Kernel, SWConv4x24Kernel},
                                     {SWConv1x32Kernel, SWConv3x32Kernel}};
#endif
  for (int b = 0; b < conv_param->output_batch_; b++) {
    for (int oh = task_id; oh < conv_param->output_h_; oh += conv_param->thread_num_) {
      float *dst_oh = output_data + oh * sw_param->out_h_step_;
      int in_h_start = sw_param->top_ * conv_param->stride_h_ - conv_param->pad_u_;
      int in_w_start = sw_param->left_ * conv_param->stride_w_ - conv_param->pad_l_;
      const float *src_h = input_data + in_h_start * sw_param->in_h_step_ + in_w_start * sw_param->ic_align_;

      int oc_block = 0;
      const float *bias = bias_data;
      for (int oc = 0; oc < sw_param->c_block_; oc += oc_block) {
        oc_block = MSMIN(C4NUM, sw_param->c_block_ - oc);  // 4 3 2 1
        const float *weight = packed_weight + oc * sw_param->kernel_step_;
        if (bias != NULL) {
          bias = bias_data + oc * oc_tile_;
        }
        float *dst_w = dst_oh + oc * oc_tile_;
        const SWConvKernel kernel_border = kernel[oc_block - 1][0];
        if (oh < sw_param->top_ || oh >= sw_param->bottom_) {  // oh in up or down border
          SWBorder(dst_w, input_data, weight, bias, oh, oh + 1, 0, conv_param->output_w_, conv_param, sw_param,
                   kernel_border, act_type, 1, oc_block);
        } else {  // oh in center
          // ow in right
          SWBorder(dst_w, input_data, weight, bias, oh, oh + 1, 0, sw_param->left_, conv_param, sw_param, kernel_border,
                   act_type, 1, oc_block);
          // ow in center
          const float *src_w = src_h + (oh - sw_param->top_) * sw_param->in_sh_step_;
          int ow_block = ow_block_num[oc_block - 1];                               // 12 6 4 3
          for (int ow = sw_param->left_; ow < sw_param->right_; ow += ow_block) {  // left ~ right
            ow_block = MSMIN(ow_block, sw_param->right_ - ow);
            if (ow_block < ow_block_num[oc_block - 1]) {  // ow is not enough and process one ow
              ow_block = 1;
            }
            kernel[oc_block - 1][ow_block > 1](dst_w + ow * sw_param->block_channel_, src_w, weight, bias, kernel_h,
                                               kernel_w, act_type, ow_block, oc_block, oc_algin, ic_algin, in_kw_step,
                                               in_kh_step, in_sw_step, 0);
            src_w += ow_block * in_sw_step;
          }
          // ow in left
          SWBorder(dst_w, input_data, weight, bias, oh, oh + 1, sw_param->right_, conv_param->output_w_, conv_param,
                   sw_param, kernel_border, act_type, 1, oc_block);
        }
      }
    }  // output h loop
    input_data += sw_param->in_step_;
    output_data += sw_param->out_step_;
  }  // batch loop
}
#ifndef ENABLE_DEBUG
void SWConv3x32Kernel(float *dst, const float *src, const float *weight, const float *bias, size_t kernel_h,
                      size_t kernel_w, size_t act_flag, size_t ow_block, size_t oc_block, size_t oc_algin,
                      size_t ic_algin, size_t in_kw_step, size_t in_kh_step, size_t in_sw_step, size_t kw_remainder) {
  in_kh_step *= 4;
  in_sw_step *= 4;
  in_kw_step *= 4;
  oc_algin *= 4;
  kw_remainder *= 4;
  asm volatile(
    "cmpq $0, %2\n"
    "je 0f\n"
    "vmovups (%2), %%ymm0\n"
    "vmovups 0x20(%2), %%ymm1\n"
    "vmovups 0x40(%2), %%ymm2\n"
    "vmovups 0x60(%2), %%ymm3\n"
    "vmovups (%2), %%ymm4\n"
    "vmovups 0x20(%2), %%ymm5\n"
    "vmovups 0x40(%2), %%ymm6\n"
    "vmovups 0x60(%2), %%ymm7\n"
    "vmovups (%2), %%ymm8\n"
    "vmovups 0x20(%2), %%ymm9\n"
    "vmovups 0x40(%2), %%ymm10\n"
    "vmovups 0x60(%2), %%ymm11\n"
    "jmp 1f\n"
    "0:\n"
    "vxorps %%ymm0, %%ymm0, %%ymm0\n"
    "vxorps %%ymm1, %%ymm1, %%ymm1\n"
    "vxorps %%ymm2, %%ymm2, %%ymm2\n"
    "vxorps %%ymm3, %%ymm3, %%ymm3\n"
    "vxorps %%ymm4, %%ymm4, %%ymm4\n"
    "vxorps %%ymm5, %%ymm5, %%ymm5\n"
    "vxorps %%ymm6, %%ymm6, %%ymm6\n"
    "vxorps %%ymm7, %%ymm7, %%ymm7\n"
    "vxorps %%ymm8, %%ymm8, %%ymm8\n"
    "vxorps %%ymm9, %%ymm9, %%ymm9\n"
    "vxorps %%ymm10, %%ymm10, %%ymm10\n"
    "vxorps %%ymm11, %%ymm11, %%ymm11\n"
    "1:\n"              // LoopH
    "movq %4, %%rsi\n"  // width
    "movq %0, %%rcx\n"  // src_h
    "2:\n"              // LoopW
    "movq %%rcx, %%rdx\n"
    "movq %5, %%r12\n"  // ic_algin
    "3:\n"              // LoopIC
    "vbroadcastss (%%rdx), %%ymm13\n"
    "vbroadcastss (%%rdx, %8), %%ymm14\n"
    "vbroadcastss (%%rdx, %8, 2), %%ymm15\n"
    "vmovups (%1), %%ymm12\n"
    "vfmadd231ps %%ymm12, %%ymm13, %%ymm0\n"
    "vfmadd231ps %%ymm12, %%ymm14, %%ymm4\n"
    "vfmadd231ps %%ymm12, %%ymm15, %%ymm8\n"
    "vmovups 0x20(%1), %%ymm12\n"
    "vfmadd231ps %%ymm12, %%ymm13, %%ymm1\n"
    "vfmadd231ps %%ymm12, %%ymm14, %%ymm5\n"
    "vfmadd231ps %%ymm12, %%ymm15, %%ymm9\n"
    "vmovups 0x40(%1), %%ymm12\n"
    "vfmadd231ps %%ymm12, %%ymm13, %%ymm2\n"
    "vfmadd231ps %%ymm12, %%ymm14, %%ymm6\n"
    "vfmadd231ps %%ymm12, %%ymm15, %%ymm10\n"
    "vmovups 0x60(%1), %%ymm12\n"
    "vfmadd231ps %%ymm12, %%ymm13, %%ymm3\n"
    "vfmadd231ps %%ymm12, %%ymm14, %%ymm7\n"
    "vfmadd231ps %%ymm12, %%ymm15, %%ymm11\n"
    "addq $128, %1\n"
    "addq $4, %%rdx\n"
    "dec %%r12\n"
    "jg 3b\n"

    "addq %6, %%rcx\n"  // in_kw_step
    "dec %%rsi\n"
    "jg 2b\n"

    "addq %7, %0\n"  // in_kh_step
    "addq %9, %1\n"
    "dec %3\n"
    "jg 1b\n"
    :
    : "r"(src), "r"(weight), "r"(bias), "r"(kernel_h), "r"(kernel_w), "r"(ic_algin), "r"(in_kw_step),  // 6
      "r"(in_kh_step), "r"(in_sw_step), "r"(kw_remainder)                                              // 9
    : "%rcx", "%rdx", "%rsi", "%r12", "%ymm0", "%ymm1", "%ymm2", "%ymm3", "%ymm4", "%ymm5", "%ymm6", "%ymm7", "%ymm8",
      "%ymm9", "%ymm10", "%ymm11", "%ymm12", "%ymm13", "%ymm14", "%ymm15");

  asm volatile(
    "and $0x3, %%eax\n"
    "je 0f\n"
    // Relu
    "vxorps %%ymm12, %%ymm12, %%ymm12\n"
    "vmaxps %%ymm12, %%ymm0, %%ymm0\n"
    "vmaxps %%ymm12, %%ymm1, %%ymm1\n"
    "vmaxps %%ymm12, %%ymm2, %%ymm2\n"
    "vmaxps %%ymm12, %%ymm3, %%ymm3\n"
    "vmaxps %%ymm12, %%ymm4, %%ymm4\n"
    "vmaxps %%ymm12, %%ymm5, %%ymm5\n"
    "vmaxps %%ymm12, %%ymm6, %%ymm6\n"
    "vmaxps %%ymm12, %%ymm7, %%ymm7\n"
    "vmaxps %%ymm12, %%ymm8, %%ymm8\n"
    "vmaxps %%ymm12, %%ymm9, %%ymm9\n"
    "vmaxps %%ymm12, %%ymm10, %%ymm10\n"
    "vmaxps %%ymm12, %%ymm11, %%ymm11\n"

    "and $0x1, %%eax\n"
    "je 0f\n"
    // relu6
    "mov $0x40C00000, %%ecx\n"
    "vmovd %%ecx, %%xmm14\n"
    "vpermps %%ymm14, %%ymm12, %%ymm14\n"
    "vminps %%ymm14, %%ymm0, %%ymm0\n"
    "vminps %%ymm14, %%ymm1, %%ymm1\n"
    "vminps %%ymm14, %%ymm2, %%ymm2\n"
    "vminps %%ymm14, %%ymm3, %%ymm3\n"
    "vminps %%ymm14, %%ymm4, %%ymm4\n"
    "vminps %%ymm14, %%ymm5, %%ymm5\n"
    "vminps %%ymm14, %%ymm6, %%ymm6\n"
    "vminps %%ymm14, %%ymm7, %%ymm7\n"
    "vminps %%ymm14, %%ymm8, %%ymm8\n"
    "vminps %%ymm14, %%ymm9, %%ymm9\n"
    "vminps %%ymm14, %%ymm10, %%ymm10\n"
    "vminps %%ymm14, %%ymm11, %%ymm11\n"

    "0:\n"
    "vmovups %%ymm0, (%2)\n"  // dst_0
    "vmovups %%ymm1, 0x20(%2)\n"
    "vmovups %%ymm2, 0x40(%2)\n"
    "vmovups %%ymm3, 0x60(%2)\n"
    "vmovups %%ymm4, (%2, %1, 1)\n"
    "vmovups %%ymm5, 0x20(%2, %1, 1)\n"
    "vmovups %%ymm6, 0x40(%2, %1, 1)\n"
    "vmovups %%ymm7, 0x60(%2, %1, 1)\n"
    "vmovups %%ymm8, (%2, %1, 2)\n"
    "vmovups %%ymm9, 0x20(%2, %1, 2)\n"
    "vmovups %%ymm10, 0x40(%2, %1, 2)\n"
    "vmovups %%ymm11, 0x60(%2, %1, 2)\n"
    :
    : "a"(act_flag), "r"(oc_algin), "r"(dst)
    : "%ecx", "%ymm0", "%ymm1", "%ymm2", "%ymm3", "%ymm4", "%ymm5", "%ymm6", "%ymm7", "%ymm8", "%ymm9", "%ymm10",
      "%ymm11", "%ymm12", "%ymm14");
}

void SWConv1x32Kernel(float *dst, const float *src, const float *weight, const float *bias, size_t kernel_h,
                      size_t kernel_w, size_t act_flag, size_t ow_block, size_t oc_block, size_t oc_algin,
                      size_t ic_algin, size_t in_kw_step, size_t in_kh_step, size_t in_sw_step, size_t kw_remainder) {
  in_kh_step *= 4;
  in_kw_step *= 4;
  oc_algin *= 4;
  kw_remainder *= 4;
  asm volatile(
    "cmpq $0, %2\n"
    "je 0f\n"
    "vmovups (%2), %%ymm0\n"
    "vmovups 0x20(%2), %%ymm1\n"
    "vmovups 0x40(%2), %%ymm2\n"
    "vmovups 0x60(%2), %%ymm3\n"
    "jmp 1f\n"
    "0:\n"
    "vxorps %%ymm0, %%ymm0, %%ymm0\n"
    "vxorps %%ymm1, %%ymm1, %%ymm1\n"
    "vxorps %%ymm2, %%ymm2, %%ymm2\n"
    "vxorps %%ymm3, %%ymm3, %%ymm3\n"
    "1:\n"              // LoopH
    "movq %4, %%rsi\n"  // width
    "movq %0, %%rcx\n"  // src_h
    "2:\n"              // Loopw
    "movq %%rcx, %%rdx\n"
    "movq %5, %%r12\n"  // ic_algin
    "3:\n"              // LoopIC
    "vbroadcastss (%%rdx), %%ymm4\n"
    // Weight data is loaded directly from memory instead of into registers for calculation.
    "vfmadd231ps (%1), %%ymm4, %%ymm0\n"
    "vfmadd231ps 0x20(%1), %%ymm4, %%ymm1\n"
    "vfmadd231ps 0x40(%1), %%ymm4, %%ymm2\n"
    "vfmadd231ps 0x60(%1), %%ymm4, %%ymm3\n"
    "addq $128, %1\n"
    "addq $4, %%rdx\n"
    "dec %%r12\n"
    "jg 3b\n"

    "addq %6, %%rcx\n"  // in_kw_step
    "dec %%rsi\n"
    "jg 2b\n"

    "addq %7, %0\n"  // in_kh_step
    "addq %8, %1\n"
    "dec %3\n"
    "jg 1b\n"
    :
    : "r"(src), "r"(weight), "r"(bias), "r"(kernel_h), "r"(kernel_w), "r"(ic_algin), "r"(in_kw_step),  // 6
      "r"(in_kh_step), "r"(kw_remainder)                                                               // 8
    : "%rcx", "%rdx", "%rsi", "%r12", "%ymm0", "%ymm1", "%ymm2", "%ymm3", "%ymm12", "%ymm13", "%ymm14", "%ymm15");

  asm volatile(
    "and $0x3, %%eax\n"
    "je 0f\n"
    // Relu
    "vxorps %%ymm12, %%ymm12, %%ymm12\n"
    "vmaxps %%ymm12, %%ymm0, %%ymm0\n"
    "vmaxps %%ymm12, %%ymm1, %%ymm1\n"
    "vmaxps %%ymm12, %%ymm2, %%ymm2\n"
    "vmaxps %%ymm12, %%ymm3, %%ymm3\n"

    "and $0x1, %%eax\n"
    "je 0f\n"
    // relu6
    "mov $0x40C00000, %%ecx\n"
    "vmovd %%ecx, %%xmm14\n"
    "vpermps %%ymm14, %%ymm12, %%ymm14\n"
    "vminps %%ymm14, %%ymm0, %%ymm0\n"
    "vminps %%ymm14, %%ymm1, %%ymm1\n"
    "vminps %%ymm14, %%ymm2, %%ymm2\n"
    "vminps %%ymm14, %%ymm3, %%ymm3\n"

    "0:\n"
    "vmovups %%ymm0, (%2)\n"  // dst_0
    "vmovups %%ymm1, 0x20(%2)\n"
    "vmovups %%ymm2, 0x40(%2)\n"
    "vmovups %%ymm3, 0x60(%2)\n"
    :
    : "a"(act_flag), "r"(oc_algin), "r"(dst)
    : "%ecx", "%ymm0", "%ymm1", "%ymm2", "%ymm3", "%ymm12", "%ymm14");
}

void SWConv4x24Kernel(float *dst, const float *src, const float *weight, const float *bias, size_t kernel_h,
                      size_t kernel_w, size_t act_flag, size_t ow_block, size_t oc_block, size_t oc_algin,
                      size_t ic_algin, size_t in_kw_step, size_t in_kh_step, size_t in_sw_step, size_t kw_remainder) {
  in_kh_step *= 4;
  in_kw_step *= 4;
  in_sw_step *= 4;
  kw_remainder *= 4;
  size_t src_3_step = 3 * in_sw_step;
  float *dst_3 = dst + 3 * oc_algin;
  oc_algin *= 4;
  asm volatile(
    "cmpq $0, %2\n"
    "je 0f\n"
    "vmovups (%2), %%ymm0\n"
    "vmovups 0x20(%2), %%ymm1\n"
    "vmovups 0x40(%2), %%ymm2\n"
    // We need to copy ymm0 to ymm3 to reduce IO time, but unfortunately I didn't find the corresponding instruction.
    "vmovups (%2), %%ymm3\n"
    "vmovups 0x20(%2), %%ymm4\n"
    "vmovups 0x40(%2), %%ymm5\n"
    "vmovups (%2), %%ymm6\n"
    "vmovups 0x20(%2), %%ymm7\n"
    "vmovups 0x40(%2), %%ymm8\n"
    "vmovups (%2), %%ymm9\n"
    "vmovups 0x20(%2), %%ymm10\n"
    "vmovups 0x40(%2), %%ymm11\n"
    "jmp 1f\n"
    "0:\n"
    "vxorps %%ymm0, %%ymm0, %%ymm0\n"
    "vxorps %%ymm1, %%ymm1, %%ymm1\n"
    "vxorps %%ymm2, %%ymm2, %%ymm2\n"
    "vxorps %%ymm3, %%ymm3, %%ymm3\n"
    "vxorps %%ymm4, %%ymm4, %%ymm4\n"
    "vxorps %%ymm5, %%ymm5, %%ymm5\n"
    "vxorps %%ymm6, %%ymm6, %%ymm6\n"
    "vxorps %%ymm7, %%ymm7, %%ymm7\n"
    "vxorps %%ymm8, %%ymm8, %%ymm8\n"
    "vxorps %%ymm9, %%ymm9, %%ymm9\n"
    "vxorps %%ymm10, %%ymm10, %%ymm10\n"
    "vxorps %%ymm11, %%ymm11, %%ymm11\n"
    "1:\n"              // LoopH
    "movq %4, %%rsi\n"  // width
    "movq %0, %%rcx\n"  // src_h
    "2:\n"              // LoopW
    "movq %%rcx, %%rdx\n"
    "movq %5, %%r12\n"  // ic_algin
    "3:\n"              // LoopIC
    "vmovups (%1), %%ymm12\n"
    "vmovups 0x20(%1), %%ymm13\n"
    "vmovups 0x40(%1), %%ymm14\n"

    "vbroadcastss (%%rdx), %%ymm15\n"
    "vfmadd231ps %%ymm15, %%ymm12, %%ymm0\n"
    "vfmadd231ps %%ymm15, %%ymm13, %%ymm1\n"
    "vfmadd231ps %%ymm15, %%ymm14, %%ymm2\n"

    "vbroadcastss (%%rdx, %8), %%ymm15\n"
    "vfmadd231ps %%ymm15, %%ymm12, %%ymm3\n"
    "vfmadd231ps %%ymm15, %%ymm13, %%ymm4\n"
    "vfmadd231ps %%ymm15, %%ymm14, %%ymm5\n"

    "vbroadcastss (%%rdx, %8, 2), %%ymm15\n"
    "vfmadd231ps %%ymm15, %%ymm12, %%ymm6\n"
    "vfmadd231ps %%ymm15, %%ymm13, %%ymm7\n"
    "vfmadd231ps %%ymm15, %%ymm14, %%ymm8\n"

    "addq %10, %%rdx\n"  // src_3
    "vbroadcastss (%%rdx), %%ymm15\n"
    "vfmadd231ps %%ymm15, %%ymm12, %%ymm9\n"
    "vfmadd231ps %%ymm15, %%ymm13, %%ymm10\n"
    "vfmadd231ps %%ymm15, %%ymm14, %%ymm11\n"

    "subq %10, %%rdx\n"
    "addq $96, %1\n"
    "addq $4, %%rdx\n"
    "dec %%r12\n"
    "jg 3b\n"

    "addq %6, %%rcx\n"  // in_kw_step
    "dec %%rsi\n"
    "jg 2b\n"

    "addq %7, %0\n"  // in_kh_step
    "addq %9, %1\n"  // border in sw need to add remainder data
    "dec %3\n"
    "jg 1b\n"
    :
    : "r"(src), "r"(weight), "r"(bias), "r"(kernel_h), "r"(kernel_w), "r"(ic_algin), "r"(in_kw_step),  // 6
      "r"(in_kh_step), "r"(in_sw_step), "r"(kw_remainder), "r"(src_3_step)                             // 10
    : "%rcx", "%rdx", "%rsi", "%r12", "%ymm0", "%ymm1", "%ymm2", "%ymm3", "%ymm4", "%ymm5", "%ymm6", "%ymm7", "%ymm8",
      "%ymm9", "%ymm10", "%ymm11", "%ymm12", "%ymm13", "%ymm14", "%ymm15");

  asm volatile(
    "and $0x3, %%eax\n"
    "je 0f\n"
    // Relu
    "vxorps %%ymm12, %%ymm12, %%ymm12\n"
    "vmaxps %%ymm12, %%ymm0, %%ymm0\n"
    "vmaxps %%ymm12, %%ymm1, %%ymm1\n"
    "vmaxps %%ymm12, %%ymm2, %%ymm2\n"
    "vmaxps %%ymm12, %%ymm3, %%ymm3\n"
    "vmaxps %%ymm12, %%ymm4, %%ymm4\n"
    "vmaxps %%ymm12, %%ymm5, %%ymm5\n"
    "vmaxps %%ymm12, %%ymm6, %%ymm6\n"
    "vmaxps %%ymm12, %%ymm7, %%ymm7\n"
    "vmaxps %%ymm12, %%ymm8, %%ymm8\n"
    "vmaxps %%ymm12, %%ymm9, %%ymm9\n"
    "vmaxps %%ymm12, %%ymm10, %%ymm10\n"
    "vmaxps %%ymm12, %%ymm11, %%ymm11\n"

    "and $0x1, %%eax\n"
    "je 0f\n"
    // relu6
    "mov $0x40C00000, %%ecx\n"
    "vmovd %%ecx, %%xmm14\n"
    "vpermps %%ymm14, %%ymm12, %%ymm14\n"
    "vminps %%ymm14, %%ymm0, %%ymm0\n"
    "vminps %%ymm14, %%ymm1, %%ymm1\n"
    "vminps %%ymm14, %%ymm2, %%ymm2\n"
    "vminps %%ymm14, %%ymm3, %%ymm3\n"
    "vminps %%ymm14, %%ymm4, %%ymm4\n"
    "vminps %%ymm14, %%ymm5, %%ymm5\n"
    "vminps %%ymm14, %%ymm6, %%ymm6\n"
    "vminps %%ymm14, %%ymm7, %%ymm7\n"
    "vminps %%ymm14, %%ymm8, %%ymm8\n"
    "vminps %%ymm14, %%ymm9, %%ymm9\n"
    "vminps %%ymm14, %%ymm10, %%ymm10\n"
    "vminps %%ymm14, %%ymm11, %%ymm11\n"

    "0:\n"
    "vmovups %%ymm0, (%2)\n"  // dst_0
    "vmovups %%ymm1, 0x20(%2)\n"
    "vmovups %%ymm2, 0x40(%2)\n"
    "vmovups %%ymm3, (%2, %1, 1)\n"
    "vmovups %%ymm4, 0x20(%2, %1, 1)\n"
    "vmovups %%ymm5, 0x40(%2, %1, 1)\n"
    "vmovups %%ymm6, (%2, %1, 2)\n"
    "vmovups %%ymm7, 0x20(%2, %1, 2)\n"
    "vmovups %%ymm8, 0x40(%2, %1, 2)\n"
    "vmovups %%ymm9, (%3)\n"  // dst+3
    "vmovups %%ymm10, 0x20(%3)\n"
    "vmovups %%ymm11, 0x40(%3)\n"
    :
    : "a"(act_flag), "r"(oc_algin), "r"(dst), "r"(dst_3)
    : "%ecx", "%ymm0", "%ymm1", "%ymm2", "%ymm3", "%ymm4", "%ymm5", "%ymm6", "%ymm7", "%ymm8", "%ymm9", "%ymm10",
      "%ymm11", "%ymm12", "%ymm14");
}

void SWConv1x24Kernel(float *dst, const float *src, const float *weight, const float *bias, size_t kernel_h,
                      size_t kernel_w, size_t act_flag, size_t ow_block, size_t oc_block, size_t oc_algin,
                      size_t ic_algin, size_t in_kw_step, size_t in_kh_step, size_t in_sw_step, size_t kw_remainder) {
  in_kh_step *= 4;
  in_kw_step *= 4;
  kw_remainder *= 4;
  oc_algin *= 4;
  asm volatile(
    "cmpq $0, %2\n"
    "je 0f\n"
    "vmovups (%2), %%ymm0\n"
    "vmovups 0x20(%2), %%ymm1\n"
    "vmovups 0x40(%2), %%ymm2\n"
    "jmp 1f\n"
    "0:\n"
    "vxorps %%ymm0, %%ymm0, %%ymm0\n"
    "vxorps %%ymm1, %%ymm1, %%ymm1\n"
    "vxorps %%ymm2, %%ymm2, %%ymm2\n"
    "1:\n"              // LoopH
    "movq %4, %%rsi\n"  // width
    "movq %0, %%rcx\n"  // src_h
    "2:\n"              // LoopW
    "movq %%rcx, %%rdx\n"
    "movq %5, %%r12\n"  // ic_algin
    "3:\n"              // LoopIC
    "vbroadcastss (%%rdx), %%ymm3\n"
    // Weight data is loaded directly from memory instead of into registers for calculation.
    "vfmadd231ps (%1), %%ymm3, %%ymm0\n"
    "vfmadd231ps 0x20(%1), %%ymm3, %%ymm1\n"
    "vfmadd231ps 0x40(%1), %%ymm3, %%ymm2\n"
    "addq $96, %1\n"
    "addq $4, %%rdx\n"
    "dec %%r12\n"
    "jg 3b\n"

    "addq %6, %%rcx\n"  // in_kw_step
    "dec %%rsi\n"
    "jg 2b\n"

    "addq %7, %0\n"  // in_kh_step
    "addq %8, %1\n"  // border in sw need to add remainder data
    "dec %3\n"
    "jg 1b\n"
    :
    : "r"(src), "r"(weight), "r"(bias), "r"(kernel_h), "r"(kernel_w), "r"(ic_algin), "r"(in_kw_step),  // 6
      "r"(in_kh_step), "r"(kw_remainder)                                                               // 8
    : "%rcx", "%rdx", "%rsi", "%r12", "%ymm0", "%ymm1", "%ymm2", "%ymm3");

  asm volatile(
    "and $0x3, %%eax\n"
    "je 0f\n"
    // Relu
    "vxorps %%ymm12, %%ymm12, %%ymm12\n"
    "vmaxps %%ymm12, %%ymm0, %%ymm0\n"
    "vmaxps %%ymm12, %%ymm1, %%ymm1\n"
    "vmaxps %%ymm12, %%ymm2, %%ymm2\n"

    "and $0x1, %%eax\n"
    "je 0f\n"
    // relu6
    "mov $0x40C00000, %%ecx\n"
    "vmovd %%ecx, %%xmm14\n"
    "vpermps %%ymm14, %%ymm12, %%ymm14\n"
    "vminps %%ymm14, %%ymm0, %%ymm0\n"
    "vminps %%ymm14, %%ymm1, %%ymm1\n"
    "vminps %%ymm14, %%ymm2, %%ymm2\n"

    "0:\n"
    "vmovups %%ymm0, (%2)\n"  // dst_0
    "vmovups %%ymm1, 0x20(%2)\n"
    "vmovups %%ymm2, 0x40(%2)\n"
    :
    : "a"(act_flag), "r"(oc_algin), "r"(dst)
    : "%ecx", "%ymm0", "%ymm1", "%ymm2", "%ymm12", "%ymm14");
}

void SWConv6x16Kernel(float *dst, const float *src, const float *weight, const float *bias, size_t kernel_h,
                      size_t kernel_w, size_t act_flag, size_t ow_block, size_t oc_block, size_t oc_algin,
                      size_t ic_algin, size_t in_kw_step, size_t in_kh_step, size_t in_sw_step, size_t kw_remainder) {
  in_kh_step *= 4;
  in_kw_step *= 4;
  in_sw_step *= 4;
  kw_remainder *= 4;
  size_t src_3_step = 3 * in_sw_step;
  float *dst_3 = dst + 3 * oc_algin;
  oc_algin *= 4;
  asm volatile(
    "cmpq $0, %2\n"
    "je 0f\n"
    "vmovups (%2), %%ymm0\n"
    "vmovups 0x20(%2), %%ymm1\n"
    // We need to copy ymm0 to ymm3 to reduce IO time, but unfortunately I didn't find the corresponding instruction.
    "vmovups (%2), %%ymm2\n"
    "vmovups 0x20(%2), %%ymm3\n"
    "vmovups (%2), %%ymm4\n"
    "vmovups 0x20(%2), %%ymm5\n"
    "vmovups (%2), %%ymm6\n"
    "vmovups 0x20(%2), %%ymm7\n"
    "vmovups (%2), %%ymm8\n"
    "vmovups 0x20(%2), %%ymm9\n"
    "vmovups (%2), %%ymm10\n"
    "vmovups 0x20(%2), %%ymm11\n"
    "jmp 1f\n"
    "0:\n"
    "vxorps %%ymm0, %%ymm0, %%ymm0\n"
    "vxorps %%ymm1, %%ymm1, %%ymm1\n"
    "vxorps %%ymm2, %%ymm2, %%ymm2\n"
    "vxorps %%ymm3, %%ymm3, %%ymm3\n"
    "vxorps %%ymm4, %%ymm4, %%ymm4\n"
    "vxorps %%ymm5, %%ymm5, %%ymm5\n"
    "vxorps %%ymm6, %%ymm6, %%ymm6\n"
    "vxorps %%ymm7, %%ymm7, %%ymm7\n"
    "vxorps %%ymm8, %%ymm8, %%ymm8\n"
    "vxorps %%ymm9, %%ymm9, %%ymm9\n"
    "vxorps %%ymm10, %%ymm10, %%ymm10\n"
    "vxorps %%ymm11, %%ymm11, %%ymm11\n"
    "1:\n"              // LoopH
    "movq %4, %%rsi\n"  // width
    "movq %0, %%rcx\n"  // src_h
    "2:\n"              // LoopW
    "movq %%rcx, %%rdx\n"
    "movq %5, %%r12\n"  // ic_algin
    "3:\n"              // LoopIC
    "vmovups (%1), %%ymm12\n"
    "vmovups 0x20(%1), %%ymm13\n"

    "vbroadcastss (%%rdx), %%ymm15\n"
    "vfmadd231ps %%ymm15, %%ymm12, %%ymm0\n"
    "vfmadd231ps %%ymm15, %%ymm13, %%ymm1\n"

    "vbroadcastss (%%rdx, %8), %%ymm15\n"
    "vfmadd231ps %%ymm15, %%ymm12, %%ymm2\n"
    "vfmadd231ps %%ymm15, %%ymm13, %%ymm3\n"

    "vbroadcastss (%%rdx, %8, 2), %%ymm15\n"
    "vfmadd231ps %%ymm15, %%ymm12, %%ymm4\n"
    "vfmadd231ps %%ymm15, %%ymm13, %%ymm5\n"

    "addq %10, %%rdx\n"  // src_3
    "vbroadcastss (%%rdx), %%ymm15\n"
    "vfmadd231ps %%ymm15, %%ymm12, %%ymm6\n"
    "vfmadd231ps %%ymm15, %%ymm13, %%ymm7\n"

    "vbroadcastss (%%rdx, %8), %%ymm15\n"
    "vfmadd231ps %%ymm15, %%ymm12, %%ymm8\n"
    "vfmadd231ps %%ymm15, %%ymm13, %%ymm9\n"

    "vbroadcastss (%%rdx, %8, 2), %%ymm15\n"
    "vfmadd231ps %%ymm15, %%ymm12, %%ymm10\n"
    "vfmadd231ps %%ymm15, %%ymm13, %%ymm11\n"

    "subq %10, %%rdx\n"
    "addq $64, %1\n"
    "addq $4, %%rdx\n"
    "dec %%r12\n"
    "jg 3b\n"

    "addq %6, %%rcx\n"  // in_kw_step
    "dec %%rsi\n"
    "jg 2b\n"

    "addq %7, %0\n"  // in_kh_step
    "addq %9, %1\n"  // border in sw need to add remainder data
    "dec %3\n"
    "jg 1b\n"
    :
    : "r"(src), "r"(weight), "r"(bias), "r"(kernel_h), "r"(kernel_w), "r"(ic_algin), "r"(in_kw_step),  // 6
      "r"(in_kh_step), "r"(in_sw_step), "r"(kw_remainder), "r"(src_3_step)                             // 10
    : "%rcx", "%rdx", "%rsi", "%r12", "%ymm0", "%ymm1", "%ymm2", "%ymm3", "%ymm4", "%ymm5", "%ymm6", "%ymm7", "%ymm8",
      "%ymm9", "%ymm10", "%ymm11", "%ymm12", "%ymm13", "%ymm14", "%ymm15");

  asm volatile(
    "and $0x3, %%eax\n"
    "je 0f\n"
    // Relu
    "vxorps %%ymm12, %%ymm12, %%ymm12\n"
    "vmaxps %%ymm12, %%ymm0, %%ymm0\n"
    "vmaxps %%ymm12, %%ymm1, %%ymm1\n"
    "vmaxps %%ymm12, %%ymm2, %%ymm2\n"
    "vmaxps %%ymm12, %%ymm3, %%ymm3\n"
    "vmaxps %%ymm12, %%ymm4, %%ymm4\n"
    "vmaxps %%ymm12, %%ymm5, %%ymm5\n"
    "vmaxps %%ymm12, %%ymm6, %%ymm6\n"
    "vmaxps %%ymm12, %%ymm7, %%ymm7\n"
    "vmaxps %%ymm12, %%ymm8, %%ymm8\n"
    "vmaxps %%ymm12, %%ymm9, %%ymm9\n"
    "vmaxps %%ymm12, %%ymm10, %%ymm10\n"
    "vmaxps %%ymm12, %%ymm11, %%ymm11\n"

    "and $0x1, %%eax\n"
    "je 0f\n"
    // relu6
    "mov $0x40C00000, %%ecx\n"
    "vmovd %%ecx, %%xmm14\n"
    "vpermps %%ymm14, %%ymm12, %%ymm14\n"
    "vminps %%ymm14, %%ymm0, %%ymm0\n"
    "vminps %%ymm14, %%ymm1, %%ymm1\n"
    "vminps %%ymm14, %%ymm2, %%ymm2\n"
    "vminps %%ymm14, %%ymm3, %%ymm3\n"
    "vminps %%ymm14, %%ymm4, %%ymm4\n"
    "vminps %%ymm14, %%ymm5, %%ymm5\n"
    "vminps %%ymm14, %%ymm6, %%ymm6\n"
    "vminps %%ymm14, %%ymm7, %%ymm7\n"
    "vminps %%ymm14, %%ymm8, %%ymm8\n"
    "vminps %%ymm14, %%ymm9, %%ymm9\n"
    "vminps %%ymm14, %%ymm10, %%ymm10\n"
    "vminps %%ymm14, %%ymm11, %%ymm11\n"

    "0:\n"
    "vmovups %%ymm0, (%2)\n"  // dst_0
    "vmovups %%ymm1, 0x20(%2)\n"
    "vmovups %%ymm2, (%2, %1, 1)\n"
    "vmovups %%ymm3, 0x20(%2, %1, 1)\n"
    "vmovups %%ymm4, (%2, %1, 2)\n"
    "vmovups %%ymm5, 0x20(%2, %1, 2)\n"
    "vmovups %%ymm6, (%3)\n"  // dst+3
    "vmovups %%ymm7, 0x20(%3)\n"
    "vmovups %%ymm8, (%3, %1, 1)\n"
    "vmovups %%ymm9, 0x20(%3, %1, 1)\n"
    "vmovups %%ymm10, (%3, %1, 2)\n"
    "vmovups %%ymm11, 0x20(%3, %1, 2)\n"
    :
    : "a"(act_flag), "r"(oc_algin), "r"(dst), "r"(dst_3)
    : "%ecx", "%ymm0", "%ymm1", "%ymm2", "%ymm3", "%ymm4", "%ymm5", "%ymm6", "%ymm7", "%ymm8", "%ymm9", "%ymm10",
      "%ymm11", "%ymm12", "%ymm14");
}

void SWConv1x16Kernel(float *dst, const float *src, const float *weight, const float *bias, size_t kernel_h,
                      size_t kernel_w, size_t act_flag, size_t ow_block, size_t oc_block, size_t oc_algin,
                      size_t ic_algin, size_t in_kw_step, size_t in_kh_step, size_t in_sw_step, size_t kw_remainder) {
  in_kh_step *= 4;
  in_kw_step *= 4;
  kw_remainder *= 4;
  oc_algin *= 4;
  asm volatile(
    "cmpq $0, %2\n"
    "je 0f\n"
    "vmovups (%2), %%ymm0\n"
    "vmovups 0x20(%2), %%ymm1\n"
    "jmp 1f\n"
    "0:\n"
    "vxorps %%ymm0, %%ymm0, %%ymm0\n"
    "vxorps %%ymm1, %%ymm1, %%ymm1\n"
    "1:\n"              // LoopH
    "movq %4, %%rsi\n"  // width
    "movq %0, %%rcx\n"  // src_h
    "2:\n"              // LoopW
    "movq %%rcx, %%rdx\n"
    "movq %5, %%r12\n"  // ic_algin
    "3:\n"              // LoopIC
    "vbroadcastss (%%rdx), %%ymm3\n"
    // Weight data is loaded directly from memory instead of into registers for calculation.
    "vfmadd231ps (%1), %%ymm3, %%ymm0\n"
    "vfmadd231ps 0x20(%1), %%ymm3, %%ymm1\n"
    "addq $64, %1\n"
    "addq $4, %%rdx\n"
    "dec %%r12\n"
    "jg 3b\n"

    "addq %6, %%rcx\n"  // in_kw_step
    "dec %%rsi\n"
    "jg 2b\n"

    "addq %7, %0\n"  // in_kh_step
    "addq %8, %1\n"  // border in sw need to add remainder data
    "dec %3\n"
    "jg 1b\n"
    :
    : "r"(src), "r"(weight), "r"(bias), "r"(kernel_h), "r"(kernel_w), "r"(ic_algin), "r"(in_kw_step),  // 6
      "r"(in_kh_step), "r"(kw_remainder)                                                               // 8
    : "%rcx", "%rdx", "%rsi", "%r12", "%ymm0", "%ymm1", "%ymm3");

  asm volatile(
    "and $0x3, %%eax\n"
    "je 0f\n"
    // Relu
    "vxorps %%ymm12, %%ymm12, %%ymm12\n"
    "vmaxps %%ymm12, %%ymm0, %%ymm0\n"
    "vmaxps %%ymm12, %%ymm1, %%ymm1\n"

    "and $0x1, %%eax\n"
    "je 0f\n"
    // relu6
    "mov $0x40C00000, %%ecx\n"
    "vmovd %%ecx, %%xmm14\n"
    "vpermps %%ymm14, %%ymm12, %%ymm14\n"
    "vminps %%ymm14, %%ymm0, %%ymm0\n"
    "vminps %%ymm14, %%ymm1, %%ymm1\n"

    "0:\n"
    "vmovups %%ymm0, (%2)\n"  // dst_0
    "vmovups %%ymm1, 0x20(%2)\n"
    :
    : "a"(act_flag), "r"(oc_algin), "r"(dst)
    : "%ecx", "%ymm0", "%ymm1", "%ymm12", "%ymm14");
}

void SWConv12x8Kernel(float *dst, const float *src, const float *weight, const float *bias, size_t kernel_h,
                      size_t kernel_w, size_t act_flag, size_t ow_block, size_t oc_block, size_t oc_algin,
                      size_t ic_algin, size_t in_kw_step, size_t in_kh_step, size_t in_sw_step, size_t kw_remainder) {
  in_kh_step *= 4;
  in_sw_step *= 4;
  in_kw_step *= 4;
  kw_remainder *= 4;
  size_t src_3_step = 3 * in_sw_step;
  float *dst_3 = dst + 3 * oc_algin;
  float *dst_5 = dst + 5 * oc_algin;
  float *dst_9 = dst + 9 * oc_algin;
  oc_algin *= 4;
  asm volatile(
    "cmpq $0, %0\n"
    "je 0f\n"
    "vmovups (%0), %%ymm0\n"
    "vmovups (%0), %%ymm1\n"
    "vmovups (%0), %%ymm2\n"
    "vmovups (%0), %%ymm3\n"
    "vmovups (%0), %%ymm4\n"
    "vmovups (%0), %%ymm5\n"
    "vmovups (%0), %%ymm6\n"
    "vmovups (%0), %%ymm7\n"
    "vmovups (%0), %%ymm8\n"
    "vmovups (%0), %%ymm9\n"
    "vmovups (%0), %%ymm10\n"
    "vmovups (%0), %%ymm11\n"
    "jmp 1f\n"
    "0:\n"
    "vxorps %%ymm0, %%ymm0, %%ymm0\n"
    "vxorps %%ymm1, %%ymm1, %%ymm1\n"
    "vxorps %%ymm2, %%ymm2, %%ymm2\n"
    "vxorps %%ymm3, %%ymm3, %%ymm3\n"
    "vxorps %%ymm4, %%ymm4, %%ymm4\n"
    "vxorps %%ymm5, %%ymm5, %%ymm5\n"
    "vxorps %%ymm6, %%ymm6, %%ymm6\n"
    "vxorps %%ymm7, %%ymm7, %%ymm7\n"
    "vxorps %%ymm8, %%ymm8, %%ymm8\n"
    "vxorps %%ymm9, %%ymm9, %%ymm9\n"
    "vxorps %%ymm10, %%ymm10, %%ymm10\n"
    "vxorps %%ymm11, %%ymm11, %%ymm11\n"
    "1:\n"
    :
    : "r"(bias)
    : "%ymm0", "%ymm1", "%ymm2", "%ymm3", "%ymm4", "%ymm5", "%ymm6", "%ymm7", "%ymm8", "%ymm9", "%ymm10", "%ymm11");

  asm volatile(
    "LoopH:\n"
    "movq %3, %%rsi\n"  // width
    "movq %0, %%rcx\n"  // src_h
    "LoopW:\n"
    "movq %%rcx, %%rdx\n"
    "movq %4, %%r12\n"  // ic_algin
    "LoopIC:\n"
    "vmovups (%1), %%ymm12\n"
    "movq %%rdx, %%rax\n"
    "addq $32, %1\n"
    "vbroadcastss (%%rax), %%ymm13\n"
    "vbroadcastss (%%rax, %7), %%ymm14\n"
    "vbroadcastss (%%rax, %7, 2), %%ymm15\n"
    "vfmadd231ps %%ymm12, %%ymm13, %%ymm0\n"
    "vfmadd231ps %%ymm12, %%ymm14, %%ymm1\n"
    "vfmadd231ps %%ymm12, %%ymm15, %%ymm2\n"
    "addq %8, %%rax\n"
    "vbroadcastss (%%rax), %%ymm13\n"
    "vbroadcastss (%%rax, %7), %%ymm14\n"
    "vbroadcastss (%%rax, %7, 2), %%ymm15\n"
    "vfmadd231ps %%ymm12, %%ymm13, %%ymm3\n"
    "vfmadd231ps %%ymm12, %%ymm14, %%ymm4\n"
    "vfmadd231ps %%ymm12, %%ymm15, %%ymm5\n"
    "addq %8, %%rax\n"
    "vbroadcastss (%%rax), %%ymm13\n"
    "vbroadcastss (%%rax, %7), %%ymm14\n"
    "vbroadcastss (%%rax, %7, 2), %%ymm15\n"
    "vfmadd231ps %%ymm12, %%ymm13, %%ymm6\n"
    "vfmadd231ps %%ymm12, %%ymm14, %%ymm7\n"
    "vfmadd231ps %%ymm12, %%ymm15, %%ymm8\n"
    "addq %8, %%rax\n"
    "vbroadcastss (%%rax), %%ymm13\n"
    "vbroadcastss (%%rax, %7), %%ymm14\n"
    "vbroadcastss (%%rax, %7, 2), %%ymm15\n"
    "vfmadd231ps %%ymm12, %%ymm13, %%ymm9\n"
    "vfmadd231ps %%ymm12, %%ymm14, %%ymm10\n"
    "vfmadd231ps %%ymm12, %%ymm15, %%ymm11\n"

    "addq $4, %%rdx\n"
    "dec %%r12\n"
    "jg LoopIC\n"

    "addq %5, %%rcx\n"  // in_kw_step
    "dec %%rsi\n"
    "jg LoopW\n"

    "addq %6, %0\n"  // in_kh_step
    "addq %9, %1\n"  // border in sw need to add remainder data
    "dec %2\n"
    "jg LoopH\n"
    :
    : "r"(src), "r"(weight), "r"(kernel_h), "r"(kernel_w), "r"(ic_algin), "r"(in_kw_step), "r"(in_kh_step),  // 6
      "r"(in_sw_step), "r"(src_3_step), "r"(kw_remainder)                                                    // 9
    : "%rcx", "%rdx", "%rsi", "%r12", "%rax", "%ymm0", "%ymm1", "%ymm2", "%ymm3", "%ymm4", "%ymm5", "%ymm6", "%ymm7",
      "%ymm8", "%ymm9", "%ymm10", "%ymm11", "%ymm12", "%ymm13", "%ymm14", "%ymm15");

  asm volatile(
    "and $0x3, %%eax\n"
    "je Write\n"
    // Relu
    "vxorps %%ymm12, %%ymm12, %%ymm12\n"
    "vmaxps %%ymm12, %%ymm0, %%ymm0\n"
    "vmaxps %%ymm12, %%ymm1, %%ymm1\n"
    "vmaxps %%ymm12, %%ymm2, %%ymm2\n"
    "vmaxps %%ymm12, %%ymm3, %%ymm3\n"
    "vmaxps %%ymm12, %%ymm4, %%ymm4\n"
    "vmaxps %%ymm12, %%ymm5, %%ymm5\n"
    "vmaxps %%ymm12, %%ymm6, %%ymm6\n"
    "vmaxps %%ymm12, %%ymm7, %%ymm7\n"
    "vmaxps %%ymm12, %%ymm8, %%ymm8\n"
    "vmaxps %%ymm12, %%ymm9, %%ymm9\n"
    "vmaxps %%ymm12, %%ymm10, %%ymm10\n"
    "vmaxps %%ymm12, %%ymm11, %%ymm11\n"

    "and $0x1, %%eax\n"
    "je Write\n"
    // relu6
    "mov $0x40C00000, %%ecx\n"
    "vmovd %%ecx, %%xmm14\n"
    "vpermps %%ymm14, %%ymm12, %%ymm14\n"
    "vminps %%ymm14, %%ymm0, %%ymm0\n"
    "vminps %%ymm14, %%ymm1, %%ymm1\n"
    "vminps %%ymm14, %%ymm2, %%ymm2\n"
    "vminps %%ymm14, %%ymm3, %%ymm3\n"
    "vminps %%ymm14, %%ymm4, %%ymm4\n"
    "vminps %%ymm14, %%ymm5, %%ymm5\n"
    "vminps %%ymm14, %%ymm6, %%ymm6\n"
    "vminps %%ymm14, %%ymm7, %%ymm7\n"
    "vminps %%ymm14, %%ymm8, %%ymm8\n"
    "vminps %%ymm14, %%ymm9, %%ymm9\n"
    "vminps %%ymm14, %%ymm10, %%ymm10\n"
    "vminps %%ymm14, %%ymm11, %%ymm11\n"

    "Write:\n"
    "vmovups %%ymm0, (%2)\n"  // dst_0
    "vmovups %%ymm1, (%2, %1)\n"
    "vmovups %%ymm2, (%2, %1, 2)\n"
    "vmovups %%ymm3, (%3)\n"  // dst_3
    "vmovups %%ymm4, (%2, %1, 4)\n"
    "vmovups %%ymm5, (%4)\n"  // dst_5
    "vmovups %%ymm6, (%4, %1, 1)\n"
    "vmovups %%ymm7, (%4, %1, 2)\n"
    "vmovups %%ymm8, (%2, %1, 8)\n"
    "vmovups %%ymm9, (%5)\n"  // dst_9
    "vmovups %%ymm10, (%5, %1, 1)\n"
    "vmovups %%ymm11, (%5, %1, 2)\n"
    :
    : "a"(act_flag), "r"(oc_algin), "r"(dst), "r"(dst_3), "r"(dst_5), "r"(dst_9)
    : "%ecx", "%ymm0", "%ymm1", "%ymm2", "%ymm3", "%ymm4", "%ymm5", "%ymm6", "%ymm7", "%ymm8", "%ymm9", "%ymm10",
      "%ymm11", "%ymm12", "%ymm14");
}

void SWConv4x8Kernel(float *dst, const float *src, const float *weight, const float *bias, size_t kernel_h,
                     size_t kernel_w, size_t act_flag, size_t ow_block, size_t oc_block, size_t oc_algin,
                     size_t ic_algin, size_t in_kw_step, size_t in_kh_step, size_t in_sw_step, size_t kw_remainder) {
  in_kh_step *= 4;
  in_sw_step *= 4;
  in_kw_step *= 4;
  size_t src_step = 3 * in_sw_step;
  float *dst_3 = dst + 3 * oc_algin;
  oc_algin *= 4;
  asm volatile(
    "cmpq $0, %2\n"
    "je 0f\n"
    "vmovups (%2), %%ymm0\n"
    "vmovups (%2), %%ymm1\n"
    "vmovups (%2), %%ymm2\n"
    "vmovups (%2), %%ymm3\n"
    "jmp 1f\n"
    "0:\n"
    "vxorps %%ymm0, %%ymm0, %%ymm0\n"
    "vxorps %%ymm1, %%ymm1, %%ymm1\n"
    "vxorps %%ymm2, %%ymm2, %%ymm2\n"
    "vxorps %%ymm3, %%ymm3, %%ymm3\n"
    "1:\n"              // LoopH
    "movq %4, %%rsi\n"  // width
    "movq %0, %%rcx\n"  // src_h
    "2:\n"              // LoopW
    "movq %%rcx, %%rdx\n"
    "movq %5, %%r12\n"  // ic_algin
    "3:\n"              // LoopIC
    "vmovups (%1), %%ymm12\n"
    "movq %%rdx, %%rax\n"
    "addq $32, %1\n"
    "vbroadcastss (%%rax), %%ymm13\n"
    "vbroadcastss (%%rax, %8), %%ymm14\n"
    "vbroadcastss (%%rax, %8, 2), %%ymm15\n"
    "vfmadd231ps %%ymm12, %%ymm13, %%ymm0\n"
    "vfmadd231ps %%ymm12, %%ymm14, %%ymm1\n"
    "vfmadd231ps %%ymm12, %%ymm15, %%ymm2\n"
    "addq %9, %%rax\n"
    "vbroadcastss (%%rax), %%ymm13\n"
    "vfmadd231ps %%ymm12, %%ymm13, %%ymm3\n"

    "addq $4, %%rdx\n"
    "dec %%r12\n"
    "jg 3b\n"

    "addq %6, %%rcx\n"  // in_kw_step
    "dec %%rsi\n"
    "jg 2b\n"

    "addq %7, %0\n"   // in_kh_step
    "addq %10, %1\n"  // border in sw need to add remainder data
    "dec %3\n"
    "jg 1b\n"
    :
    : "r"(src), "r"(weight), "r"(bias), "r"(kernel_h), "r"(kernel_w), "r"(ic_algin), "r"(in_kw_step),  // 6
      "r"(in_kh_step), "r"(in_sw_step), "r"(src_step), "r"(kw_remainder)                               // 10
    : "%rcx", "%rdx", "%rsi", "%r12", "%ymm0", "%ymm1", "%ymm2", "%ymm3", "%ymm12", "%ymm13", "%ymm14", "%ymm15");

  asm volatile(
    "and $0x3, %%eax\n"
    "je 0f\n"
    // Relu
    "vxorps %%ymm12, %%ymm12, %%ymm12\n"
    "vmaxps %%ymm12, %%ymm0, %%ymm0\n"
    "vmaxps %%ymm12, %%ymm1, %%ymm1\n"
    "vmaxps %%ymm12, %%ymm2, %%ymm2\n"
    "vmaxps %%ymm12, %%ymm3, %%ymm3\n"

    "and $0x1, %%eax\n"
    "je 0f\n"
    // relu6
    "mov $0x40C00000, %%ecx\n"
    "vmovd %%ecx, %%xmm14\n"
    "vpermps %%ymm14, %%ymm12, %%ymm14\n"
    "vminps %%ymm14, %%ymm0, %%ymm0\n"
    "vminps %%ymm14, %%ymm1, %%ymm1\n"
    "vminps %%ymm14, %%ymm2, %%ymm2\n"
    "vminps %%ymm14, %%ymm3, %%ymm3\n"

    "0:\n"
    "vmovups %%ymm0, (%2)\n"  // dst_0
    "vmovups %%ymm1, (%2, %1)\n"
    "vmovups %%ymm2, (%2, %1, 2)\n"
    "vmovups %%ymm3, (%3)\n"  // dst_3
    :
    : "a"(act_flag), "r"(oc_algin), "r"(dst), "r"(dst_3)
    : "%ecx", "%ymm0", "%ymm1", "%ymm2", "%ymm3", "%ymm4", "%ymm12", "%ymm14");
}

void SWConv1x8Kernel(float *dst, const float *src, const float *weight, const float *bias, size_t kernel_h,
                     size_t kernel_w, size_t act_flag, size_t ow_block, size_t oc_block, size_t oc_algin,
                     size_t ic_algin, size_t in_kw_step, size_t in_kh_step, size_t in_sw_step, size_t kw_remainder) {
  in_kh_step *= 4;
  in_kw_step *= 4;
  kw_remainder *= 4;
  oc_algin *= 4;
  asm volatile(
    "cmpq $0, %2\n"
    "je 0f\n"
    "vmovups (%2), %%ymm0\n"
    "jmp 1f\n"
    "0:\n"
    "vxorps %%ymm0, %%ymm0, %%ymm0\n"
    "1:\n"              // LoopH
    "movq %4, %%rsi\n"  // width
    "movq %0, %%rcx\n"  // src_h
    "2:\n"              // LoopW
    "movq %%rcx, %%rdx\n"
    "movq %5, %%r12\n"  // ic_algin
    "3:\n"              // LoopIC
    "vbroadcastss (%%rdx), %%ymm1\n"
    // Weight data is loaded directly from memory instead of into registers for calculation.
    "vfmadd231ps (%1), %%ymm1, %%ymm0\n"
    "addq $32, %1\n"
    "addq $4, %%rdx\n"
    "dec %%r12\n"
    "jg 3b\n"

    "addq %6, %%rcx\n"  // in_kw_step
    "dec %%rsi\n"
    "jg 2b\n"

    "addq %7, %0\n"  // in_kh_step
    "addq %8, %1\n"  // border in sw need to add remainder data
    "dec %3\n"
    "jg 1b\n"
    :
    : "r"(src), "r"(weight), "r"(bias), "r"(kernel_h), "r"(kernel_w), "r"(ic_algin), "r"(in_kw_step),  // 6
      "r"(in_kh_step), "r"(kw_remainder)                                                               // 8
    : "%rcx", "%rdx", "%rsi", "%r12", "%ymm0", "%ymm1");

  asm volatile(
    "and $0x3, %%eax\n"
    "je 0f\n"
    // Relu
    "vxorps %%ymm12, %%ymm12, %%ymm12\n"
    "vmaxps %%ymm12, %%ymm0, %%ymm0\n"

    "and $0x1, %%eax\n"
    "je 0f\n"
    // relu6
    "mov $0x40C00000, %%ecx\n"
    "vmovd %%ecx, %%xmm14\n"
    "vpermps %%ymm14, %%ymm12, %%ymm14\n"
    "vminps %%ymm14, %%ymm0, %%ymm0\n"

    "0:\n"
    "vmovups %%ymm0, (%2)\n"  // dst_0
    :
    : "a"(act_flag), "r"(oc_algin), "r"(dst)
    : "%ecx", "%ymm0", "%ymm12", "%ymm14");
}
#endif

#ifdef ENABLE_DEBUG
void SWConvWxKKernel(float *dst, const float *src, const float *weight, const float *bias, size_t kernel_h,
                     size_t kernel_w, size_t act_flag, size_t ow_block, size_t oc_block, size_t oc_algin,
                     size_t ic_algin, size_t in_kw_step, size_t in_kh_step, size_t in_sw_step, size_t kw_remainder) {
  __m256 dst_data[12];
  const float *src_kh[12];
  const float *src_kw[12];
  __m256 weight_data[4];
  for (int i = 0; i < ow_block; ++i) {
    if (bias != NULL) {
      for (int j = 0; j < oc_block; ++j) {
        dst_data[i * oc_block + j] = _mm256_loadu_ps(bias + j * 8);
      }
    } else {
      for (int j = 0; j < oc_block; ++j) {
        dst_data[i * oc_block + j] = _mm256_set1_ps(0.0f);
      }
    }
    src_kh[i] = src + i * in_sw_step;
    src_kw[i] = NULL;
  }
  const float *weight_kernel = weight;
  for (int kh = 0; kh < kernel_h; kh++) {
    for (int i = 0; i < ow_block; ++i) {
      src_kw[i] = src_kh[i];
    }
    for (int kw = 0; kw < kernel_w; kw++) {
      for (int ic = 0; ic < ic_algin; ++ic) {
        for (int j = 0; j < oc_block; ++j) {
          weight_data[j] = _mm256_loadu_ps(weight_kernel + j * C8NUM);
        }
        for (int i = 0; i < ow_block; ++i) {
          for (int j = 0; j < oc_block; ++j) {
            dst_data[i * oc_block + j] += src_kw[i][ic] * weight_data[j];
          }
        }
        weight_kernel += C8NUM * oc_block;
      }  // ic loop
      for (int i = 0; i < ow_block; ++i) {
        src_kw[i] += in_kw_step;
      }
    }  // kernel_w loop
    weight_kernel += kw_remainder;
    for (int i = 0; i < ow_block; ++i) {
      src_kh[i] += in_kh_step;
    }
  }  // kernel_h loop
  // add bias and relu
  for (int i = 0; i < ow_block; ++i) {
    for (int j = 0; j < oc_block; ++j) {
      if (0x1 & act_flag) {  // relu6
        dst_data[i * oc_block + j] = _mm256_min_ps(dst_data[i * oc_block + j], _mm256_set1_ps(6.0f));
      }
      if (0x2 & act_flag) {  // relu
        dst_data[i * oc_block + j] = _mm256_max_ps(dst_data[i * oc_block + j], _mm256_set1_ps(0.0f));
      }
      _mm256_storeu_ps(dst + i * oc_algin + j * C8NUM, dst_data[i * oc_block + j]);
    }
  }
}

void SWConvNx8Kernel(float *dst, const float *src, const float *weight, const float *bias, size_t kernel_h,
                     size_t kernel_w, size_t act_flag, size_t ow_block, size_t oc_algin, size_t ic_algin,
                     size_t in_kw_step, size_t in_kh_step, size_t in_sw_step) {
  __m256 dst_data[12];
  const float *src_kh[12];
  const float *src_kw[12];
  for (int i = 0; i < ow_block; ++i) {
    if (bias != NULL) {
      dst_data[i] = _mm256_loadu_ps(bias);
    } else {
      dst_data[i] = _mm256_set1_ps(0.0f);
    }
    src_kh[i] = src + i * in_sw_step;
    src_kw[i] = NULL;
  }
  const float *weight_kernel = weight;
  for (int kh = 0; kh < kernel_h; kh++) {
    for (int i = 0; i < ow_block; ++i) {
      src_kw[i] = src_kh[i];
    }
    for (int kw = 0; kw < kernel_w; kw++) {
      for (int ic = 0; ic < ic_algin; ++ic) {
        __m256 weight_data = _mm256_loadu_ps(weight_kernel);
        for (int i = 0; i < ow_block; ++i) {
          dst_data[i] += src_kw[i][ic] * weight_data;
        }
        weight_kernel += C8NUM;
      }  // ic loop
      for (int i = 0; i < ow_block; ++i) {
        src_kw[i] += in_kw_step;
      }
    }  // kernel_w loop
    for (int i = 0; i < ow_block; ++i) {
      src_kh[i] += in_kh_step;
    }
  }  // kernel_h loop
  // add bias and relu
  for (int i = 0; i < ow_block; ++i) {
    if (0x1 & act_flag) {  // relu6
      dst_data[i] = _mm256_min_ps(dst_data[i], _mm256_set1_ps(6.0f));
    }
    if (0x2 & act_flag) {  // relu
      dst_data[i] = _mm256_max_ps(dst_data[i], _mm256_set1_ps(0.0f));
    }
    _mm256_storeu_ps(dst + i * oc_algin, dst_data[i]);
  }
}

void SWConv1x8Kernel11(float *dst, const float *src, const float *weight, const float *bias, size_t kernel_h,
                       size_t kernel_w, size_t act_flag, size_t ow_block, size_t oc_block, size_t oc_algin,
                       size_t ic_algin, size_t in_kw_step, size_t in_kh_step, size_t in_sw_step, size_t kw_remainder) {
  __m256 dst_data;
  const float *src_kh = src;
  const float *src_kw = NULL;
  if (bias != NULL) {
    dst_data = _mm256_loadu_ps(bias);
  } else {
    dst_data = _mm256_set1_ps(0.0f);
  }
  const float *weight_kernel = weight;
  for (int kh = 0; kh < kernel_h; kh++) {
    src_kw = src_kh;
    for (int kw = 0; kw < kernel_w; kw++) {
      for (int ic = 0; ic < ic_algin; ++ic) {
        __m256 weight_data = _mm256_loadu_ps(weight_kernel);
        dst_data += src_kw[ic] * weight_data;
        weight_kernel += C8NUM;
      }  // ic loop
      src_kw += in_kw_step;
    }  // kernel_w loop
    src_kh += in_kh_step;
  }  // kernel_h loop
  // add bias and relu
  if (0x1 & act_flag) {  // relu6
    dst_data = _mm256_min_ps(dst_data, _mm256_set1_ps(6.0f));
  }
  if (0x2 & act_flag) {  // relu
    dst_data = _mm256_max_ps(dst_data, _mm256_set1_ps(0.0f));
  }
  _mm256_storeu_ps(dst, dst_data);
}
#endif
#endif
