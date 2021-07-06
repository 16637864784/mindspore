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

#include "nnacl/fp16/matmul_fp16.h"

static void Col2Row8SrcFromFp16(const void *src_ptr, float16_t *dst_ptr, size_t row, size_t col) {
  int row_c8 = row / C8NUM * C8NUM;
  int col_c8 = col / C8NUM * C8NUM;
  const float16_t *src = (const float16_t *)src_ptr;
  int ci = 0;
  for (; ci < col_c8; ci += C8NUM) {
    int ri = 0;
    for (; ri < row_c8; ri += C8NUM) {
      const float16_t *src_ptr1 = src + ci * row + ri;
      float16_t *dst_ptr1 = dst_ptr + ci * row + ri * C8NUM;
#ifdef ENABLE_ARM64
      size_t strid_row = row * 2;
      asm volatile(
        "mov x10, %[src_ptr1]\n"
        "mov x11, %[dst_ptr1]\n"
        "mov x12, %[strid_row]\n"
        "ld1 {v0.8h}, [x10], x12\n"
        "ld1 {v1.8h}, [x10], x12\n"
        "ld1 {v2.8h}, [x10], x12\n"
        "ld1 {v3.8h}, [x10], x12\n"
        "ld1 {v4.8h}, [x10], x12\n"
        "ld1 {v5.8h}, [x10], x12\n"
        "ld1 {v6.8h}, [x10], x12\n"
        "ld1 {v7.8h}, [x10], x12\n"

        "zip1 v8.8h, v0.8h, v1.8h\n"
        "zip1 v9.8h, v2.8h, v3.8h\n"
        "zip1 v10.8h, v4.8h, v5.8h\n"
        "zip1 v11.8h, v6.8h, v7.8h\n"

        "trn1 v12.4s, v8.4s, v9.4s\n"
        "trn1 v14.4s, v10.4s, v11.4s\n"
        "trn2 v13.4s, v8.4s, v9.4s\n"
        "trn2 v15.4s, v10.4s, v11.4s\n"

        "trn1 v16.2d, v12.2d, v14.2d\n"
        "trn2 v18.2d, v12.2d, v14.2d\n"
        "trn1 v17.2d, v13.2d, v15.2d\n"
        "trn2 v19.2d, v13.2d, v15.2d\n"

        "zip2 v8.8h, v0.8h, v1.8h\n"
        "zip2 v9.8h, v2.8h, v3.8h\n"
        "zip2 v10.8h, v4.8h, v5.8h\n"
        "zip2 v11.8h, v6.8h, v7.8h\n"

        "trn1 v12.4s, v8.4s, v9.4s\n"
        "trn1 v14.4s, v10.4s, v11.4s\n"
        "trn2 v13.4s, v8.4s, v9.4s\n"
        "trn2 v15.4s, v10.4s, v11.4s\n"

        "trn1 v20.2d, v12.2d, v14.2d\n"
        "trn2 v22.2d, v12.2d, v14.2d\n"
        "trn1 v21.2d, v13.2d, v15.2d\n"
        "trn2 v23.2d, v13.2d, v15.2d\n"

        "st1 {v16.8h}, [x11], #16\n"
        "st1 {v17.8h}, [x11], #16\n"
        "st1 {v18.8h}, [x11], #16\n"
        "st1 {v19.8h}, [x11], #16\n"
        "st1 {v20.8h}, [x11], #16\n"
        "st1 {v21.8h}, [x11], #16\n"
        "st1 {v22.8h}, [x11], #16\n"
        "st1 {v23.8h}, [x11], #16\n"
        :
        : [ dst_ptr1 ] "r"(dst_ptr1), [ src_ptr1 ] "r"(src_ptr1), [ strid_row ] "r"(strid_row)
        : "x10", "x11", "x12", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", "v13",
          "v14", "v15", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23");
#else
      for (int tr = 0; tr < C8NUM; ++tr) {
        for (int tc = 0; tc < C8NUM; ++tc) {
          dst_ptr1[tr * C8NUM + tc] = src_ptr1[tc * row + tr];
        }
      }
#endif
    }
    for (; ri < row; ++ri) {
      const float16_t *src_ptr1 = src + ci * row;
      float16_t *dst_ptr1 = dst_ptr + ci * row;
      for (int tc = 0; tc < C8NUM; ++tc) {
        dst_ptr1[ri * C8NUM + tc] = src_ptr1[tc * row + ri];
      }
    }
  }
  for (int r = 0; r < row; r++) {
    for (int tc = ci; tc < col; tc++) {
      int cd8 = tc / C8NUM;
      int cm8 = tc % C8NUM;
      dst_ptr[cd8 * C8NUM * row + r * C8NUM + cm8] = src[tc * row + r];
    }
  }
}

static void Col2Row8SrcFromFp32(const void *src_ptr, float16_t *dst_ptr, size_t row, size_t col) {
  int row_c8 = row / C8NUM * C8NUM;
  int col_c8 = col / C8NUM * C8NUM;
  int ci = 0;
  const float *src = (const float *)src_ptr;
  for (; ci < col_c8; ci += C8NUM) {
    int ri = 0;
    for (; ri < row_c8; ri += C8NUM) {
      const float *src_ptr1 = src + ci * row + ri;
      float16_t *dst_ptr1 = dst_ptr + ci * row + ri * C8NUM;
#ifdef ENABLE_ARM64
      size_t strid_row = row * 4;
      asm volatile(
        "mov x10, %[src_ptr1]\n"
        "mov x11, %[dst_ptr1]\n"
        "mov x12, %[strid_row]\n"
        "ld1 {v8.4s, v9.4s}, [x10], x12\n"
        "ld1 {v10.4s, v11.4s}, [x10], x12\n"
        "ld1 {v12.4s, v13.4s}, [x10], x12\n"
        "ld1 {v14.4s, v15.4s}, [x10], x12\n"
        "ld1 {v16.4s, v17.4s}, [x10], x12\n"
        "ld1 {v18.4s, v19.4s}, [x10], x12\n"
        "ld1 {v20.4s, v21.4s}, [x10], x12\n"
        "ld1 {v22.4s, v23.4s}, [x10], x12\n"

        "fcvtn v0.4h, v8.4s\n"
        "fcvtn2 v0.8h, v9.4s\n"
        "fcvtn v1.4h, v10.4s\n"
        "fcvtn2 v1.8h, v11.4s\n"
        "fcvtn v2.4h, v12.4s\n"
        "fcvtn2 v2.8h, v13.4s\n"
        "fcvtn v3.4h, v14.4s\n"
        "fcvtn2 v3.8h, v15.4s\n"
        "fcvtn v4.4h, v16.4s\n"
        "fcvtn2 v4.8h, v17.4s\n"
        "fcvtn v5.4h, v18.4s\n"
        "fcvtn2 v5.8h, v19.4s\n"
        "fcvtn v6.4h, v20.4s\n"
        "fcvtn2 v6.8h, v21.4s\n"
        "fcvtn v7.4h, v22.4s\n"
        "fcvtn2 v7.8h, v23.4s\n"

        "zip1 v8.8h, v0.8h, v1.8h\n"
        "zip1 v9.8h, v2.8h, v3.8h\n"
        "zip1 v10.8h, v4.8h, v5.8h\n"
        "zip1 v11.8h, v6.8h, v7.8h\n"

        "trn1 v12.4s, v8.4s, v9.4s\n"
        "trn1 v14.4s, v10.4s, v11.4s\n"
        "trn2 v13.4s, v8.4s, v9.4s\n"
        "trn2 v15.4s, v10.4s, v11.4s\n"

        "trn1 v16.2d, v12.2d, v14.2d\n"
        "trn2 v18.2d, v12.2d, v14.2d\n"
        "trn1 v17.2d, v13.2d, v15.2d\n"
        "trn2 v19.2d, v13.2d, v15.2d\n"

        "zip2 v8.8h, v0.8h, v1.8h\n"
        "zip2 v9.8h, v2.8h, v3.8h\n"
        "zip2 v10.8h, v4.8h, v5.8h\n"
        "zip2 v11.8h, v6.8h, v7.8h\n"

        "trn1 v12.4s, v8.4s, v9.4s\n"
        "trn1 v14.4s, v10.4s, v11.4s\n"
        "trn2 v13.4s, v8.4s, v9.4s\n"
        "trn2 v15.4s, v10.4s, v11.4s\n"

        "trn1 v20.2d, v12.2d, v14.2d\n"
        "trn2 v22.2d, v12.2d, v14.2d\n"
        "trn1 v21.2d, v13.2d, v15.2d\n"
        "trn2 v23.2d, v13.2d, v15.2d\n"

        "st1 {v16.8h}, [x11], #16\n"
        "st1 {v17.8h}, [x11], #16\n"
        "st1 {v18.8h}, [x11], #16\n"
        "st1 {v19.8h}, [x11], #16\n"
        "st1 {v20.8h}, [x11], #16\n"
        "st1 {v21.8h}, [x11], #16\n"
        "st1 {v22.8h}, [x11], #16\n"
        "st1 {v23.8h}, [x11], #16\n"
        :
        : [ dst_ptr1 ] "r"(dst_ptr1), [ src_ptr1 ] "r"(src_ptr1), [ strid_row ] "r"(strid_row)
        : "x10", "x11", "x12", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", "v13",
          "v14", "v15", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23");
#else
      for (int tr = 0; tr < C8NUM; ++tr) {
        for (int tc = 0; tc < C8NUM; ++tc) {
          dst_ptr1[tr * C8NUM + tc] = (float16_t)(src_ptr1[tc * row + tr]);
        }
      }
#endif
    }
    for (; ri < row; ++ri) {
      const float *src_ptr1 = src + ci * row;
      float16_t *dst_ptr1 = dst_ptr + ci * row;
      for (int tc = 0; tc < C8NUM; ++tc) {
        dst_ptr1[ri * C8NUM + tc] = (float16_t)(src_ptr1[tc * row + ri]);
      }
    }
  }
  for (int r = 0; r < row; r++) {
    for (int tc = ci; tc < col; tc++) {
      int cd8 = tc / C8NUM;
      int cm8 = tc % C8NUM;
      dst_ptr[cd8 * C8NUM * row + r * C8NUM + cm8] = (float16_t)(src[tc * row + r]);
    }
  }
}

void ColMajor2Row8MajorFp16(const void *src_ptr, float16_t *dst_ptr, size_t row, size_t col, bool src_float16) {
  if (src_float16) {
    Col2Row8SrcFromFp16(src_ptr, dst_ptr, row, col);
  } else {
    Col2Row8SrcFromFp32(src_ptr, dst_ptr, row, col);
  }
  return;
}

void MatMul16x8Fp16(const float16_t *a, const float16_t *b, float16_t *dst, const float16_t *bias, ActType act_type,
                    int deep, int row, int col, int stride, int write_mode) {
  if (write_mode == OutType_Nhwc) {
    for (int r = 0; r < row; r++) {
      for (int c = 0; c < col; c++) {
        int r16div = r / 16, r16mod = r % 16;
        int c8div = c / 8, c8mod = c % 8;
        size_t ci = r * stride + c;
        float16_t value = 0;
        for (int d = 0; d < deep; d++) {
          size_t ai = r16div * deep * 16 + d * 16 + r16mod;
          size_t bi = c8div * deep * 8 + d * 8 + c8mod;
          value = value + a[ai] * b[bi];
        }
        ADD_BIAS(value, bias, c)
        DO_RELU(value, act_type)
        DO_RELU6(value, act_type)
        dst[ci] = value;
      }
    }
  } else if (write_mode == OutType_C8) {
    int col_8 = UP_ROUND(col, C8NUM);
    int row_16 = UP_ROUND(row, C16NUM);
    for (int r = 0; r < row_16; r++) {
      for (int c = 0; c < col_8; c++) {
        int r16div = r / C16NUM, r16mod = r % C16NUM;
        int c8div = c / C8NUM, c8mod = c % C8NUM;
        size_t ci = (c8div * C8NUM * row_16 + r * C8NUM + c8mod);
        float16_t value = 0;
        for (int d = 0; d < deep; d++) {
          size_t ai = r16div * deep * C16NUM + d * C16NUM + r16mod;
          size_t bi = c8div * deep * C8NUM + d * C8NUM + c8mod;
          value = value + a[ai] * b[bi];
        }
        ADD_BIAS(value, bias, c)
        DO_RELU(value, act_type)
        DO_RELU6(value, act_type)
        dst[ci] = value;
      }
    }
  } else {
    for (int i = 0; i < row; ++i) {
      int src_r_offset = i;
      int dst_r_offset = i * col * stride;
      for (int j = 0; j < col; ++j) {
        int c8div = j / 8, c8mod = j % 8;
        size_t ci = dst_r_offset + c8div * 8 * stride + c8mod;
        float16_t value = 0;
        for (int d = 0; d < deep; ++d) {
          size_t ai = src_r_offset + d * C16NUM;
          size_t bi = c8div * deep * 8 + d * 8 + c8mod;
          value = value + a[ai] * b[bi];
        }
        ADD_BIAS(value, bias, j)
        DO_RELU(value, act_type)
        DO_RELU6(value, act_type)
        dst[ci] = value;
      }
    }
  }
}

void MatMul12x8Fp16(const float16_t *a, const float16_t *b, float16_t *dst, const float16_t *bias, ActType act_type,
                    int deep, int row, int col, int stride, int write_mode) {
  if (write_mode == OutType_Nhwc) {  // common conv and matmul
    for (int r = 0; r < row; r++) {
      for (int c = 0; c < col; c++) {
        int r12div = r / 12, r12mod = r % 12;
        int c8div = c / 8, c8mod = c % 8;
        size_t ci = r * stride + c;
        float16_t value = 0;
        for (int d = 0; d < deep; d++) {
          size_t ai = r12div * deep * 12 + d * 12 + r12mod;
          size_t bi = c8div * deep * 8 + d * 8 + c8mod;
          value = value + a[ai] * b[bi];
        }
        ADD_BIAS(value, bias, c)
        DO_RELU(value, act_type)
        DO_RELU6(value, act_type)
        dst[ci] = value;
      }
    }
  } else if (write_mode == OutType_C8) {  // common deconv
    int col_8 = UP_ROUND(col, C8NUM);
    int row_12 = UP_ROUND(row, C12NUM);
    for (int r = 0; r < row_12; r++) {
      for (int c = 0; c < col_8; c++) {
        int r12div = r / C12NUM, r12mod = r % C12NUM;
        int c8div = c / C8NUM, c8mod = c % C8NUM;
        size_t ci = (c8div * C8NUM * row_12 + r * C8NUM + c8mod);
        float16_t value = 0;
        for (int d = 0; d < deep; d++) {
          size_t ai = r12div * deep * C12NUM + d * C12NUM + r12mod;
          size_t bi = c8div * deep * C8NUM + d * C8NUM + c8mod;
          value = value + a[ai] * b[bi];
        }
        ADD_BIAS(value, bias, c)
        DO_RELU(value, act_type)
        DO_RELU6(value, act_type)
        dst[ci] = value;
      }
    }
  } else {  // winograd conv
    for (int i = 0; i < row; ++i) {
      int src_r_offset = i;
      int dst_r_offset = i * col * stride;
      for (int j = 0; j < col; ++j) {
        int c8div = j / 8, c8mod = j % 8;
        size_t ci = dst_r_offset + c8div * 8 * stride + c8mod;
        float16_t value = 0;
        for (int d = 0; d < deep; ++d) {
          size_t ai = src_r_offset + d * C12NUM;
          size_t bi = c8div * deep * 8 + d * 8 + c8mod;
          value = value + a[ai] * b[bi];
        }
        ADD_BIAS(value, bias, j)
        DO_RELU(value, act_type)
        DO_RELU6(value, act_type)
        dst[ci] = value;
      }
    }
  }
}

#ifdef ENABLE_DEBUG
void MatMul12x16Fp16(const float16_t *a, const float16_t *b, float16_t *dst, const float16_t *bias, ActType act_type,
                     int deep, int row, int col, size_t stride, size_t out_type) {
  for (int r = 0; r < row; r++) {
    for (int c = 0; c < col; c++) {
      int r12div = r / C12NUM, r12mod = r % C12NUM;
      int c16div = c / C16NUM, c16mod = c % C16NUM;
      size_t index = r * stride + c;
      float16_t value = 0;
      for (int d = 0; d < deep; d++) {
        size_t ai = r12div * deep * C12NUM + d * C12NUM + r12mod;
        size_t bi = c16div * deep * C16NUM + d * C16NUM + c16mod;
        value = value + a[ai] * b[bi];
      }
      ADD_BIAS(value, bias, c)
      DO_RELU(value, act_type)
      DO_RELU6(value, act_type)
      dst[index] = value;
    }
  }
}
#endif

void MatMulFp16(const float16_t *a, const float16_t *b, float16_t *c, const float16_t *bias, ActType act_type,
                int depth, int row, int col, int stride, int out_type) {
  if (out_type == OutType_C8) {
    // common deconv
#ifdef ENABLE_ARM64
    MatmulFp16Neon64(a, b, c, bias, (int)act_type, depth, row, col, stride, false);
#else
    MatMul12x8Fp16(a, b, c, bias, (int)act_type, depth, row, col, stride, out_type);
#endif
  } else {
    // winograd conv(OntType_TileC8) and common conv(OutType_Nhwc) and matmul(OutType_Nhwc)
#ifdef ENABLE_ARM64
    MatmulFp16Neon64Opt(a, b, c, bias, (int)act_type, depth, row, col, stride, out_type);
#else
    MatMul12x8A32Fp16(a, b, c, bias, (int)act_type, depth, row, col, stride, out_type);
#endif
  }
  return;
}

#ifdef ENABLE_ARM64
// 8 X 16
void VecMatmulFp16(const float16_t *a, const float16_t *b, float16_t *c, const float16_t *bias, int act_type, int depth,
                   int col) {
  int align_col = UP_ROUND(col, C16NUM);
  int ci = 0;
  for (; ci < align_col - C16NUM + 1; ci += C16NUM) {
    float16x8_t acc_0 = vdupq_n_f16((float16_t)0.0);
    float16x8_t acc_1 = vdupq_n_f16((float16_t)0.0);
    if (bias != NULL) {
      acc_0 = vld1q_f16(bias + ci);
      acc_1 = vld1q_f16(bias + ci + C8NUM);
    }
    const float16_t *bv_base = b + ci * depth;
    int di = 0;
    for (; di < depth - C8NUM + 1; di += C8NUM) {
      float16x8_t av = vld1q_f16(a + di);
      float16x8_t bv_0[C8NUM];
      float16x8_t bv_1[C8NUM];
      for (int i = 0; i < C8NUM; ++i) {
        bv_0[i] = vld1q_f16(bv_base);
        bv_1[i] = vld1q_f16(bv_base + C8NUM);
        bv_base += C16NUM;
      }
      for (int i = 0; i < C8NUM; ++i) {
        acc_0 = vfmaq_n_f16(acc_0, bv_0[i], av[i]);
        acc_1 = vfmaq_n_f16(acc_1, bv_1[i], av[i]);
      }
    }
    if (di < depth) {
      for (; di < depth; ++di) {
        float16_t ai = a[di];
        float16x8_t bv0 = vld1q_f16(bv_base);
        float16x8_t bv1 = vld1q_f16(bv_base + C8NUM);
        acc_0 = vfmaq_n_f16(acc_0, bv0, ai);
        acc_1 = vfmaq_n_f16(acc_1, bv1, ai);
        bv_base += C16NUM;
      }
    }  // only save actual col num data
    if (ci + C8NUM > col) {
      int c_remain = col - ci;
      for (int i = 0; i < c_remain; ++i) {
        if (act_type == ActType_Relu) {
          c[i] = MSMAX(acc_0[i], (float16_t)0.0);
        } else if (act_type == ActType_Relu6) {
          c[i] = MSMIN(MSMAX(acc_0[i], (float16_t)0.0), (float16_t)6.0);
        } else {
          c[i] = acc_0[i];
        }
      }
      return;
    }
    if (act_type == ActType_Relu) {
      acc_0 = vmaxq_f16(acc_0, vdupq_n_f16((float16_t)0.0));
    }
    if (act_type == ActType_Relu6) {
      acc_0 = vminq_f16(vmaxq_f16(acc_0, vdupq_n_f16((float16_t)0.0)), vdupq_n_f16((float16_t)6.0));
    }
    vst1q_f16(c, acc_0);

    if (ci + C16NUM > col) {
      int c_remain = col - ci - C8NUM;
      for (int i = 0; i < c_remain; ++i) {
        if (act_type == ActType_Relu) {
          c[C8NUM + i] = MSMAX(acc_1[i], (float16_t)0.0);
        } else if (act_type == ActType_Relu6) {
          c[C8NUM + i] = MSMIN(MSMAX(acc_1[i], (float16_t)0.0), (float16_t)6.0);
        } else {
          c[C8NUM + i] = acc_1[i];
        }
      }
      return;
    }
    if (act_type == ActType_Relu) {
      acc_1 = vmaxq_f16(acc_1, vdupq_n_f16((float16_t)0.0));
    }
    if (act_type == ActType_Relu6) {
      acc_1 = vminq_f16(vmaxq_f16(acc_1, vdupq_n_f16((float16_t)0.0)), vdupq_n_f16((float16_t)6.0));
    }
    vst1q_f16(c + C8NUM, acc_1);
    c += C16NUM;
  }
}
#endif

#ifdef ENABLE_ARM82_A32
void MatVecMulA32Fp16(const float16_t *a, const float16_t *b, float16_t *c, const float16_t *bias, int act_type,
                      int depth, int col) {
  for (int ci = 0; ci < col; ci++) {
    float value = 0;
    for (int di = 0; di < depth; di++) {
      value += a[di] * b[ci * depth + di];
    }
    if (bias != NULL) value += bias[ci];
    if (act_type == ActType_Relu6) value = MSMIN(6.0f, value);
    if (act_type == ActType_Relu || act_type == ActType_Relu6) value = MSMAX(0.0f, value);
    c[ci] = value;
  }
}
#endif

void MatVecMulFp16(const float16_t *a, const float16_t *b, float16_t *c, const float16_t *bias, ActType act_type,
                   int depth, int col) {
#ifdef ENABLE_ARM64
  MatVecMulFp16Neon64(a, b, c, bias, (int)act_type, depth, col);
#else
  MatVecMulA32NeonFp16(a, b, c, bias, (int)act_type, depth, col);
#endif
}

#ifdef ENABLE_ARM64
static void Row2Col16Block16(const float16_t *src_ptr, float16_t *dst_ptr, size_t col) {
  size_t stride = col * 2;
  asm volatile(
    "mov x10, %[src_c]\n"
    "mov x11, %[dst_c]\n"

    "ld1 {v0.8h}, [x10], %[stride]\n"
    "ld1 {v1.8h}, [x10], %[stride]\n"
    "ld1 {v2.8h}, [x10], %[stride]\n"
    "ld1 {v3.8h}, [x10], %[stride]\n"
    "ld1 {v4.8h}, [x10], %[stride]\n"
    "ld1 {v5.8h}, [x10], %[stride]\n"
    "ld1 {v6.8h}, [x10], %[stride]\n"
    "ld1 {v7.8h}, [x10], %[stride]\n"

    "zip1 v16.8h, v0.8h, v1.8h\n"
    "zip1 v17.8h, v2.8h, v3.8h\n"
    "zip1 v18.8h, v4.8h, v5.8h\n"
    "zip1 v19.8h, v6.8h, v7.8h\n"

    "ld1 {v8.8h}, [x10], %[stride]\n"
    "ld1 {v9.8h}, [x10], %[stride]\n"
    "ld1 {v10.8h}, [x10], %[stride]\n"
    "ld1 {v11.8h}, [x10], %[stride]\n"
    "ld1 {v12.8h}, [x10], %[stride]\n"
    "ld1 {v13.8h}, [x10], %[stride]\n"
    "ld1 {v14.8h}, [x10], %[stride]\n"
    "ld1 {v15.8h}, [x10], %[stride]\n"

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

    "st1 {v24.8h}, [x11], #16\n"
    "st1 {v28.8h}, [x11], #16\n"
    "st1 {v26.8h}, [x11], #16\n"
    "st1 {v30.8h}, [x11], #16\n"
    "st1 {v25.8h}, [x11], #16\n"
    "st1 {v29.8h}, [x11], #16\n"
    "st1 {v27.8h}, [x11], #16\n"
    "st1 {v31.8h}, [x11], #16\n"

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

    "st1 {v24.8h}, [x11], #16\n"
    "st1 {v28.8h}, [x11], #16\n"
    "st1 {v26.8h}, [x11], #16\n"
    "st1 {v30.8h}, [x11], #16\n"
    "st1 {v25.8h}, [x11], #16\n"
    "st1 {v29.8h}, [x11], #16\n"
    "st1 {v27.8h}, [x11], #16\n"
    "st1 {v31.8h}, [x11], #16\n"
    :
    : [ dst_c ] "r"(dst_ptr), [ src_c ] "r"(src_ptr), [ stride ] "r"(stride)
    : "x10", "x11", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", "v13", "v14",
      "v15", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30",
      "v31");
}
#endif

void RowMajor2Col16MajorFp16Opt(const float16_t *src_ptr, float16_t *dst_ptr, size_t row, size_t col) {
  size_t row_up_16 = UP_ROUND(row, C16NUM);
  size_t row16 = row / C16NUM * C16NUM;
  size_t col8 = col / C8NUM * C8NUM;
  const float16_t *src_r = src_ptr;
  float16_t *dst_r = dst_ptr;
  size_t ri = 0;
  // find 16 block unit
  for (; ri < row16; ri += C16NUM) {
    size_t ci = 0;
    for (; ci < col8; ci += C8NUM) {
      const float16_t *src_c = src_r + ci;
      float16_t *dst_c = dst_r + ci * C16NUM;
#ifdef ENABLE_ARM64
      Row2Col16Block16(src_c, dst_c, col);
#else
      for (int tr = 0; tr < C16NUM; tr++) {
        for (int tc = 0; tc < C8NUM; tc++) {
          dst_c[tc * C16NUM + tr] = src_c[tr * col + tc];
        }
      }
#endif
    }
    for (; ci < col; ci++) {
      const float16_t *src_c = src_r + ci;
      float16_t *dst_c = dst_r + ci * C16NUM;
      for (size_t i = 0; i < C16NUM; i++) {
        dst_c[i] = src_c[i * col];
      }
    }
    src_r += C16NUM * col;
    dst_r += C16NUM * col;
  }
  for (; ri < row; ri++) {
    for (size_t i = 0; i < col; ++i) {
      dst_r[i * C16NUM] = src_r[i];
    }
    src_r += col;
    dst_r += 1;
  }
  for (; ri < row_up_16; ri++) {
    for (size_t i = 0; i < col; i++) {
      dst_r[i * C16NUM] = 0;
    }
    dst_r += 1;
  }
  return;
}

void RowMajor2Col12MajorFp16Opt(const float16_t *src_ptr, float16_t *dst_ptr, size_t row, size_t col) {
  size_t row_up_12 = UP_ROUND(row, C12NUM);
  size_t row12 = row / C12NUM * C12NUM;
  size_t col8 = col / C8NUM * C8NUM;
  const float16_t *src_r = src_ptr;
  float16_t *dst_r = dst_ptr;
  size_t ri = 0;
  // transpose 12x8
  for (; ri < row12; ri += C12NUM) {
    size_t ci = 0;
    for (; ci < col8; ci += C8NUM) {
      const float16_t *src_c = src_r + ci;
      float16_t *dst_c = dst_r + ci * C12NUM;
#ifdef ENABLE_ARM64
      Transpose12x8ARM64Fp16(src_c, dst_c, col * sizeof(float16_t), 24);
#elif ENABLE_ARM82_A32
      Transpose12x8A32Fp16(src_c, dst_c, col * sizeof(float16_t), 24);
#else
      for (int tr = 0; tr < C12NUM; tr++) {
        for (int tc = 0; tc < C8NUM; tc++) {
          dst_c[tc * C12NUM + tr] = src_c[tr * col + tc];
        }
      }
#endif
    }
    for (; ci < col; ci++) {
      const float16_t *src_c = src_r + ci;
      float16_t *dst_c = dst_r + ci * C12NUM;
      for (size_t i = 0; i < C12NUM; i++) {
        dst_c[i] = src_c[i * col];
      }
    }
    src_r += C12NUM * col;
    dst_r += C12NUM * col;
  }
  for (; ri < row; ri++) {
    for (size_t i = 0; i < col; ++i) {
      dst_r[i * C12NUM] = src_r[i];
    }
    src_r += col;
    dst_r += 1;
  }
  for (; ri < row_up_12; ri++) {
    for (size_t i = 0; i < col; i++) {
      dst_r[i * C12NUM] = 0;
    }
    dst_r += 1;
  }
}

void RowMajor2Col16MajorFp16(const void *src, float16_t *dst, int row, int col, bool is_fp32_src) {
  if (is_fp32_src) {
    const float *fp32_src = (const float *)src;
    for (int r = 0; r < row; r++) {
      for (int c = 0; c < col; c++) {
        int r_div16 = r / 16;
        int r_mod16 = r % 16;
        dst[r_div16 * 16 * col + c * 16 + r_mod16] = (float16_t)(fp32_src[r * col + c]);
      }
    }
  } else {
    const float16_t *fp16_src = (const float16_t *)src;
    RowMajor2Col16MajorFp16Opt(fp16_src, dst, row, col);
  }
  return;
}

void RowMajor2Col12MajorFp16(const void *src, float16_t *dst, int row, int col, bool is_fp32_src) {
  if (is_fp32_src) {
    const float *fp32_src = (const float *)src;
    for (int r = 0; r < row; r++) {
      for (int c = 0; c < col; c++) {
        int r_div12 = r / 12;
        int r_mod12 = r % 12;
        dst[r_div12 * 12 * col + c * 12 + r_mod12] = (float16_t)(fp32_src[r * col + c]);
      }
    }
  } else {
    const float16_t *fp16_src = (const float16_t *)src;
    RowMajor2Col12MajorFp16Opt(fp16_src, dst, row, col);
  }
  return;
}

void RowMajor2Row16MajorFp16(const void *src, float16_t *dst, int row, int col, bool is_fp32_src) {
  for (int r = 0; r < row; r++) {
    for (int c = 0; c < col; c++) {
      int c_div16 = c / 16;
      int c_mod16 = c % 16;
      if (is_fp32_src) {
        dst[c_div16 * 16 * row + r * 16 + c_mod16] = (float16_t)(((const float *)src)[r * col + c]);
      } else {
        dst[c_div16 * 16 * row + r * 16 + c_mod16] = ((const float16_t *)src)[r * col + c];
      }
    }
  }
}

void RowMajor2Row16MajorFp16Opt(const float16_t *src, float16_t *dst, int row, int col) {
  int col_align = UP_ROUND(col, C16NUM);
  for (int r = 0; r < row; r++) {
    int c = 0;
    for (; c < col; c++) {
      int c_div16 = c / C16NUM;
      int c_mod16 = c % C16NUM;
      dst[c_div16 * C16NUM * row + r * C16NUM + c_mod16] = src[r * col + c];
    }
    for (; c < col_align; c++) {
      int c_div16 = c / C16NUM;
      int c_mod16 = c % C16NUM;
      dst[c_div16 * C16NUM * row + r * C16NUM + c_mod16] = (float16_t)0.0;
    }
  }
}

void RowMajor2Row12MajorFp16(const void *src, float16_t *dst, int row, int col, bool is_fp32_src) {
  for (int r = 0; r < row; r++) {
    for (int c = 0; c < col; c++) {
      int c_div12 = c / 12;
      int c_mod12 = c % 12;
      if (is_fp32_src) {
        dst[c_div12 * 12 * row + r * 12 + c_mod12] = (float16_t)(((const float *)src)[r * col + c]);
      } else {
        dst[c_div12 * 12 * row + r * 12 + c_mod12] = ((const float16_t *)src)[r * col + c];
      }
    }
  }
}

void RowMajor2Row8MajorFp16(const void *src, float16_t *dst, int row, int col, bool is_fp32_src) {
  for (int r = 0; r < row; r++) {
    for (int c = 0; c < col; c++) {
      int c_div8 = c / 8;
      int c_mod8 = c % 8;
      if (is_fp32_src) {
        dst[c_div8 * 8 * row + r * 8 + c_mod8] = (float16_t)(((const float *)src)[r * col + c]);
      } else {
        dst[c_div8 * 8 * row + r * 8 + c_mod8] = ((const float16_t *)src)[r * col + c];
      }
    }
  }
}

void RowMajor2ColMajorFp16(const void *src, float16_t *dst, int row, int col, bool is_fp32_src) {
  for (int r = 0; r < row; ++r) {
    for (int c = 0; c < col; ++c) {
      if (is_fp32_src) {
        dst[c * row + r] = (float16_t)(((const float *)src)[r * col + c]);
      } else {
        dst[c * row + r] = ((const float16_t *)src)[r * col + c];
      }
    }
  }
}

void RowMajor2Col8MajorFp16(const void *src, float16_t *dst, int row, int col, bool is_fp32_src) {
  for (int r = 0; r < row; r++) {
    for (int c = 0; c < col; c++) {
      int r_div8 = r / 8;
      int r_mod8 = r % 8;
      if (is_fp32_src) {
        dst[r_div8 * 8 * col + c * 8 + r_mod8] = (float16_t)(((const float *)src)[r * col + c]);
      } else {
        dst[r_div8 * 8 * col + c * 8 + r_mod8] = ((const float16_t *)src)[r * col + c];
      }
    }
  }
}
