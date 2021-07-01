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
#ifdef ENABLE_SSE
#include <x86intrin.h>
#endif

#ifdef ENABLE_AVX
#include <immintrin.h>
#endif

#include <math.h>
#include "nnacl/fp32/exp_fp32.h"
#include "nnacl/fp32/adam_fp32.h"

#ifdef ENABLE_AVX512
struct AVX_Data {
  __m512 data;
};

inline void LoadStep4(struct AVX_Data *inp0, const float *inp1) {
  inp0[0].data = _mm512_loadu_ps(inp1);
  inp0[1].data = _mm512_loadu_ps(inp1 + C16NUM);
  inp0[2].data = _mm512_loadu_ps(inp1 + C16NUM * 2);
  inp0[3].data = _mm512_loadu_ps(inp1 + C16NUM * 3);
}

inline void StoreStep4(float *inp0, struct AVX_Data *inp1) {
  _mm512_storeu_ps(inp0, inp1[0].data);
  _mm512_storeu_ps(inp0 + C16NUM, inp1[1].data);
  _mm512_storeu_ps(inp0 + C16NUM * 2, inp1[2].data);
  _mm512_storeu_ps(inp0 + C16NUM * 3, inp1[3].data);
}
#endif
int AdamFp32(float *var, float *m, float *v, float lr, float beta1, float beta2, float epsilon, const float *gradient,
             size_t start, size_t end, bool use_nesterov) {
  size_t c1 = start;
#ifdef ENABLE_AVX
  size_t c8 = ((end - start) / C8NUM) * C8NUM;
  __m256 coeff1_r = _mm256_set1_ps(1 - beta1);
  __m256 coeff2_r = _mm256_set1_ps(1 - beta2);
  __m256 beta1_r = _mm256_set1_ps(beta1);
  __m256 lr_r = _mm256_set1_ps(lr);
  __m256 epsi_r = _mm256_set1_ps(epsilon);

  float *var_ptr = var + start;
  float *m_ptr = m + start;
  float *v_ptr = v + start;
  const float *grad_ptr = gradient + start;

  __m256 avx_r0, avx_r1;
  __m256 var_r, m_r, v_r, grad_r;

  for (; c1 < start + c8; c1 += C8NUM) {
    grad_r = _mm256_loadu_ps(grad_ptr);
    m_r = _mm256_loadu_ps(m_ptr);
    avx_r0 = _mm256_sub_ps(grad_r, m_r);
    avx_r1 = _mm256_mul_ps(avx_r0, coeff1_r);
    m_r = _mm256_add_ps(m_r, avx_r1);
    _mm256_storeu_ps(m_ptr, m_r);

    v_r = _mm256_loadu_ps(v_ptr);
    avx_r0 = _mm256_sub_ps(_mm256_mul_ps(grad_r, grad_r), v_r);
    v_r = _mm256_add_ps(v_r, _mm256_mul_ps(avx_r0, coeff2_r));
    _mm256_storeu_ps(v_ptr, v_r);

    if (use_nesterov) {
      avx_r0 = _mm256_add_ps(_mm256_mul_ps(m_r, beta1_r), _mm256_mul_ps(coeff1_r, grad_r));
      avx_r1 = _mm256_mul_ps(lr_r, avx_r0);
      avx_r0 = _mm256_add_ps(_mm256_sqrt_ps(v_r), epsi_r);
      __m256 avx_r2 = _mm256_div_ps(avx_r1, avx_r0);

      var_r = _mm256_loadu_ps(var_ptr);
      var_r = _mm256_sub_ps(var_r, avx_r2);
      _mm256_storeu_ps(var_ptr, var_r);
    } else {
      avx_r0 = _mm256_mul_ps(lr_r, m_r);
      avx_r1 = _mm256_add_ps(_mm256_sqrt_ps(v_r), epsi_r);
      __m256 avx_r2 = _mm256_div_ps(avx_r0, avx_r1);
      var_r = _mm256_loadu_ps(var_ptr);
      var_r = _mm256_sub_ps(var_r, avx_r2);
      _mm256_storeu_ps(var_ptr, var_r);
    }
    m_ptr += C8NUM;
    v_ptr += C8NUM;
    var_ptr += C8NUM;
    grad_ptr += C8NUM;
  }
#endif

  // remaining
  for (; c1 < end; c1++) {
    m[c1] += (gradient[c1] - m[c1]) * (1 - beta1);
    v[c1] += (gradient[c1] * gradient[c1] - v[c1]) * (1 - beta2);
    if (use_nesterov) {
      var[c1] -= lr * (m[c1] * beta1 + (1 - beta1) * gradient[c1]) / (sqrt(v[c1]) + epsilon);
    } else {
      var[c1] -= lr * m[c1] / (sqrt(v[c1]) + epsilon);
    }
  }
  return NNACL_OK;
}

int AdamDeltaFp32(float *delta, float *m, float *v, float lr, float beta1, float beta2, float epsilon,
                  const float *gradient, size_t start, size_t end, bool use_nesterov) {
  size_t c1 = start;
#ifdef ENABLE_AVX
  size_t c8 = ((end - start) / C8NUM) * C8NUM;
  __m256 coeff1_r = _mm256_set1_ps(1.0f - beta1);
  __m256 coeff2_r = _mm256_set1_ps(1.0f - beta2);
  __m256 beta1_r = _mm256_set1_ps(beta1);
  __m256 beta2_r = _mm256_set1_ps(beta2);
  __m256 lr_r = _mm256_set1_ps(-lr);
  __m256 epsi_r = _mm256_set1_ps(epsilon);

  float *m_ptr = m + start;
  float *v_ptr = v + start;
  float *delta_ptr = delta + start;
  const float *gradient_ptr = gradient + start;

  __m256 m_r, v_r, delta_r, grad_r;
  __m256 avx_r0, avx_r1;
  for (; c1 < start + c8; c1 += C8NUM) {
    m_r = _mm256_loadu_ps(m_ptr);
    avx_r0 = _mm256_mul_ps(m_r, beta1_r);
    grad_r = _mm256_loadu_ps(gradient_ptr);
    m_r = _mm256_add_ps(avx_r0, _mm256_mul_ps(coeff1_r, grad_r));
    _mm256_storeu_ps(m_ptr, m_r);

    v_r = _mm256_loadu_ps(v_ptr);
    avx_r0 = _mm256_mul_ps(v_r, beta2_r);
    avx_r1 = _mm256_mul_ps(_mm256_mul_ps(coeff2_r, grad_r), grad_r);
    v_r = _mm256_add_ps(avx_r0, avx_r1);
    _mm256_storeu_ps(v_ptr, v_r);

    if (use_nesterov) {
      avx_r0 = _mm256_add_ps(_mm256_mul_ps(m_r, beta1_r), _mm256_mul_ps(coeff1_r, grad_r));
      avx_r0 = _mm256_mul_ps(lr_r, avx_r0);
      avx_r1 = _mm256_add_ps(_mm256_sqrt_ps(v_r), epsi_r);
      delta_r = _mm256_div_ps(avx_r0, avx_r1);
      _mm256_storeu_ps(delta_ptr, delta_r);
    } else {
      avx_r0 = _mm256_mul_ps(lr_r, m_r);
      avx_r1 = _mm256_add_ps(_mm256_sqrt_ps(v_r), epsi_r);
      delta_r = _mm256_div_ps(avx_r0, avx_r1);
      _mm256_storeu_ps(delta_ptr, delta_r);
    }
    m_ptr += C8NUM;
    v_ptr += C8NUM;
    delta_ptr += C8NUM;
    gradient_ptr += C8NUM;
  }
#endif

  // remaining
  for (; c1 < end; ++c1) {
    m[c1] *= beta1;
    m[c1] += (1 - beta1) * gradient[c1];
    v[c1] *= beta2;
    v[c1] += (1 - beta2) * gradient[c1] * gradient[c1];
    if (use_nesterov) {
      delta[c1] = -lr * (m[c1] * beta1 + (1 - beta1) * gradient[c1]) / (sqrt(v[c1]) + epsilon);
    } else {
      delta[c1] = -lr * m[c1] / (sqrt(v[c1]) + epsilon);
    }
  }
  return NNACL_OK;
}

int AdamWeightDecayFp32(float *var, float *m, float *v, float lr, float beta1, float beta2, float epsilon, float *decay,
                        const float *gradient, size_t start, size_t end) {
  size_t c1 = start;
#ifdef ENABLE_AVX512
  float beta1_minus = 1 - beta1;
  float beta2_minus = 1 - beta2;
  struct AVX_Data beta1_r, beta2_r, beta1_minus_r, beta2_minus_r, lr_neg_r, epsilon_r, decay_r;
  beta1_r.data = _mm512_set1_ps(beta1);
  beta2_r.data = _mm512_set1_ps(beta2);
  beta1_minus_r.data = _mm512_set1_ps(beta1_minus);
  beta2_minus_r.data = _mm512_set1_ps(beta2_minus);
  lr_neg_r.data = _mm512_set1_ps(-lr);
  epsilon_r.data = _mm512_set1_ps(epsilon);
  decay_r.data = _mm512_set1_ps(*decay);
  size_t c16 = ((end - start) / C16NUM) * C16NUM + start;
  size_t c64 = ((end - start) / C64NUM) * C64NUM + start;

  const float *gradient_ptr = gradient + start;
  float *var_ptr = var + start;
  float *m_ptr = m + start;
  float *v_ptr = v + start;

  for (; c1 < c64; c1 += C64NUM) {
    struct AVX_Data g_r[4], var_r[4], m_r[4], v_r[4];
    LoadStep4(g_r, gradient_ptr);
    LoadStep4(var_r, var_ptr);
    LoadStep4(m_r, m_ptr);
    LoadStep4(v_r, v_ptr);

    m_r[0].data = _mm512_mul_ps(m_r[0].data, beta1_r.data);
    m_r[1].data = _mm512_mul_ps(m_r[1].data, beta1_r.data);
    m_r[2].data = _mm512_mul_ps(m_r[2].data, beta1_r.data);
    m_r[3].data = _mm512_mul_ps(m_r[3].data, beta1_r.data);
    m_r[0].data = _mm512_fmadd_ps(g_r[0].data, beta1_minus_r.data, m_r[0].data);
    m_r[1].data = _mm512_fmadd_ps(g_r[1].data, beta1_minus_r.data, m_r[1].data);
    m_r[2].data = _mm512_fmadd_ps(g_r[2].data, beta1_minus_r.data, m_r[2].data);
    m_r[3].data = _mm512_fmadd_ps(g_r[3].data, beta1_minus_r.data, m_r[3].data);

    v_r[0].data = _mm512_mul_ps(v_r[0].data, beta2_r.data);
    v_r[1].data = _mm512_mul_ps(v_r[1].data, beta2_r.data);
    v_r[2].data = _mm512_mul_ps(v_r[2].data, beta2_r.data);
    v_r[3].data = _mm512_mul_ps(v_r[3].data, beta2_r.data);
    g_r[0].data = _mm512_mul_ps(g_r[0].data, g_r[0].data);
    g_r[1].data = _mm512_mul_ps(g_r[1].data, g_r[1].data);
    g_r[2].data = _mm512_mul_ps(g_r[2].data, g_r[2].data);
    g_r[3].data = _mm512_mul_ps(g_r[3].data, g_r[3].data);
    v_r[0].data = _mm512_fmadd_ps(g_r[0].data, beta2_minus_r.data, v_r[0].data);
    v_r[1].data = _mm512_fmadd_ps(g_r[1].data, beta2_minus_r.data, v_r[1].data);
    v_r[2].data = _mm512_fmadd_ps(g_r[2].data, beta2_minus_r.data, v_r[2].data);
    v_r[3].data = _mm512_fmadd_ps(g_r[3].data, beta2_minus_r.data, v_r[3].data);

    g_r[0].data = _mm512_sqrt_ps(v_r[0].data);
    g_r[1].data = _mm512_sqrt_ps(v_r[1].data);
    g_r[2].data = _mm512_sqrt_ps(v_r[2].data);
    g_r[3].data = _mm512_sqrt_ps(v_r[3].data);
    g_r[0].data = _mm512_div_ps(m_r[0].data, _mm512_add_ps(g_r[0].data, epsilon_r.data));
    g_r[1].data = _mm512_div_ps(m_r[1].data, _mm512_add_ps(g_r[1].data, epsilon_r.data));
    g_r[2].data = _mm512_div_ps(m_r[2].data, _mm512_add_ps(g_r[2].data, epsilon_r.data));
    g_r[3].data = _mm512_div_ps(m_r[3].data, _mm512_add_ps(g_r[3].data, epsilon_r.data));
    g_r[0].data = _mm512_fmadd_ps(var_r[0].data, decay_r.data, g_r[0].data);
    g_r[1].data = _mm512_fmadd_ps(var_r[1].data, decay_r.data, g_r[1].data);
    g_r[2].data = _mm512_fmadd_ps(var_r[2].data, decay_r.data, g_r[2].data);
    g_r[3].data = _mm512_fmadd_ps(var_r[3].data, decay_r.data, g_r[3].data);

    var_r[0].data = _mm512_fmadd_ps(g_r[0].data, lr_neg_r.data, var_r[0].data);
    var_r[1].data = _mm512_fmadd_ps(g_r[1].data, lr_neg_r.data, var_r[1].data);
    var_r[2].data = _mm512_fmadd_ps(g_r[2].data, lr_neg_r.data, var_r[2].data);
    var_r[3].data = _mm512_fmadd_ps(g_r[3].data, lr_neg_r.data, var_r[3].data);

    StoreStep4(var_ptr, var_r);
    StoreStep4(m_ptr, m_r);
    StoreStep4(v_ptr, v_r);

    gradient_ptr += C64NUM;
    var_ptr += C64NUM;
    m_ptr += C64NUM;
    v_ptr += C64NUM;
  }
  for (; c1 < c16; c1 += C16NUM) {
    struct AVX_Data g_r, var_r, m_r, v_r;
    g_r.data = _mm512_loadu_ps(gradient_ptr);
    var_r.data = _mm512_loadu_ps(var_ptr);
    m_r.data = _mm512_loadu_ps(m_ptr);
    v_r.data = _mm512_loadu_ps(v_ptr);

    m_r.data = _mm512_mul_ps(m_r.data, beta1_r.data);
    v_r.data = _mm512_mul_ps(v_r.data, beta2_r.data);
    struct AVX_Data avx_r0;
    avx_r0.data = _mm512_mul_ps(g_r.data, g_r.data);
    m_r.data = _mm512_fmadd_ps(g_r.data, beta1_minus_r.data, m_r.data);
    v_r.data = _mm512_fmadd_ps(avx_r0.data, beta2_minus_r.data, v_r.data);
    avx_r0.data = _mm512_sqrt_ps(v_r.data);
    avx_r0.data = _mm512_div_ps(m_r.data, _mm512_add_ps(avx_r0.data, epsilon_r.data));
    avx_r0.data = _mm512_fmadd_ps(var_r.data, decay_r.data, avx_r0.data);
    var_r.data = _mm512_fmadd_ps(avx_r0.data, lr_neg_r.data, var_r.data);
    _mm512_storeu_ps(var_ptr, var_r.data);
    _mm512_storeu_ps(m_ptr, m_r.data);
    _mm512_storeu_ps(v_ptr, v_r.data);

    gradient_ptr += C16NUM;
    var_ptr += C16NUM;
    m_ptr += C16NUM;
    v_ptr += C16NUM;
  }
#endif
  return c1;
}

int FusedAdamFp32(float *var, float *m, float *v, float lr, float beta1, float beta2, float epsilon, float *decay,
                  const int16_t *gradient16, size_t start, size_t end) {
  size_t c1 = start;
#ifdef ENABLE_AVX512
  float beta1_minus = 1 - beta1;
  float beta2_minus = 1 - beta2;
  struct AVX_Data beta1_r, beta2_r, beta1_minus_r, beta2_minus_r, lr_neg_r, epsilon_r, decay_r;
  beta1_r.data = _mm512_set1_ps(beta1);
  beta2_r.data = _mm512_set1_ps(beta2);
  beta1_minus_r.data = _mm512_set1_ps(beta1_minus);
  beta2_minus_r.data = _mm512_set1_ps(beta2_minus);
  lr_neg_r.data = _mm512_set1_ps(-lr);
  epsilon_r.data = _mm512_set1_ps(epsilon);
  decay_r.data = _mm512_set1_ps(*decay);
  size_t c16 = ((end - start) / C16NUM) * C16NUM + start;
  size_t c64 = ((end - start) / C64NUM) * C64NUM + start;

  const int16_t *gradient16_ptr = gradient16 + start;
  float *var_ptr = var + start;
  float *m_ptr = m + start;
  float *v_ptr = v + start;

  for (; c1 < c64; c1 += C64NUM) {
    struct AVX_Data g_r[4], var_r[4], m_r[4], v_r[4];
    g_r[0].data = _mm512_cvtph_ps(_mm256_loadu_si256((__m256i *)(gradient16_ptr)));
    g_r[1].data = _mm512_cvtph_ps(_mm256_loadu_si256((__m256i *)(gradient16_ptr + C16NUM)));
    g_r[2].data = _mm512_cvtph_ps(_mm256_loadu_si256((__m256i *)(gradient16_ptr + C16NUM * 2)));
    g_r[3].data = _mm512_cvtph_ps(_mm256_loadu_si256((__m256i *)(gradient16_ptr + C16NUM * 3)));

    LoadStep4(var_r, var_ptr);
    LoadStep4(m_r, m_ptr);
    LoadStep4(v_r, v_ptr);

    m_r[0].data = _mm512_mul_ps(m_r[0].data, beta1_r.data);
    m_r[1].data = _mm512_mul_ps(m_r[1].data, beta1_r.data);
    m_r[2].data = _mm512_mul_ps(m_r[2].data, beta1_r.data);
    m_r[3].data = _mm512_mul_ps(m_r[3].data, beta1_r.data);
    m_r[0].data = _mm512_fmadd_ps(g_r[0].data, beta1_minus_r.data, m_r[0].data);
    m_r[1].data = _mm512_fmadd_ps(g_r[1].data, beta1_minus_r.data, m_r[1].data);
    m_r[2].data = _mm512_fmadd_ps(g_r[2].data, beta1_minus_r.data, m_r[2].data);
    m_r[3].data = _mm512_fmadd_ps(g_r[3].data, beta1_minus_r.data, m_r[3].data);

    v_r[0].data = _mm512_mul_ps(v_r[0].data, beta2_r.data);
    v_r[1].data = _mm512_mul_ps(v_r[1].data, beta2_r.data);
    v_r[2].data = _mm512_mul_ps(v_r[2].data, beta2_r.data);
    v_r[3].data = _mm512_mul_ps(v_r[3].data, beta2_r.data);
    g_r[0].data = _mm512_mul_ps(g_r[0].data, g_r[0].data);
    g_r[1].data = _mm512_mul_ps(g_r[1].data, g_r[1].data);
    g_r[2].data = _mm512_mul_ps(g_r[2].data, g_r[2].data);
    g_r[3].data = _mm512_mul_ps(g_r[3].data, g_r[3].data);
    v_r[0].data = _mm512_fmadd_ps(g_r[0].data, beta2_minus_r.data, v_r[0].data);
    v_r[1].data = _mm512_fmadd_ps(g_r[1].data, beta2_minus_r.data, v_r[1].data);
    v_r[2].data = _mm512_fmadd_ps(g_r[2].data, beta2_minus_r.data, v_r[2].data);
    v_r[3].data = _mm512_fmadd_ps(g_r[3].data, beta2_minus_r.data, v_r[3].data);

    g_r[0].data = _mm512_sqrt_ps(v_r[0].data);
    g_r[1].data = _mm512_sqrt_ps(v_r[1].data);
    g_r[2].data = _mm512_sqrt_ps(v_r[2].data);
    g_r[3].data = _mm512_sqrt_ps(v_r[3].data);
    g_r[0].data = _mm512_div_ps(m_r[0].data, _mm512_add_ps(g_r[0].data, epsilon_r.data));
    g_r[1].data = _mm512_div_ps(m_r[1].data, _mm512_add_ps(g_r[1].data, epsilon_r.data));
    g_r[2].data = _mm512_div_ps(m_r[2].data, _mm512_add_ps(g_r[2].data, epsilon_r.data));
    g_r[3].data = _mm512_div_ps(m_r[3].data, _mm512_add_ps(g_r[3].data, epsilon_r.data));
    g_r[0].data = _mm512_fmadd_ps(var_r[0].data, decay_r.data, g_r[0].data);
    g_r[1].data = _mm512_fmadd_ps(var_r[1].data, decay_r.data, g_r[1].data);
    g_r[2].data = _mm512_fmadd_ps(var_r[2].data, decay_r.data, g_r[2].data);
    g_r[3].data = _mm512_fmadd_ps(var_r[3].data, decay_r.data, g_r[3].data);

    var_r[0].data = _mm512_fmadd_ps(g_r[0].data, lr_neg_r.data, var_r[0].data);
    var_r[1].data = _mm512_fmadd_ps(g_r[1].data, lr_neg_r.data, var_r[1].data);
    var_r[2].data = _mm512_fmadd_ps(g_r[2].data, lr_neg_r.data, var_r[2].data);
    var_r[3].data = _mm512_fmadd_ps(g_r[3].data, lr_neg_r.data, var_r[3].data);

    StoreStep4(var_ptr, var_r);
    StoreStep4(m_ptr, m_r);
    StoreStep4(v_ptr, v_r);

    gradient16_ptr += C64NUM;
    var_ptr += C64NUM;
    m_ptr += C64NUM;
    v_ptr += C64NUM;
  }
  for (; c1 < c16; c1 += C16NUM) {
    struct AVX_Data g_r, var_r, m_r, v_r;
    g_r.data = _mm512_cvtph_ps(_mm256_loadu_si256((__m256i *)(gradient16_ptr)));
    var_r.data = _mm512_loadu_ps(var_ptr);
    m_r.data = _mm512_loadu_ps(m_ptr);
    v_r.data = _mm512_loadu_ps(v_ptr);

    m_r.data = _mm512_mul_ps(m_r.data, beta1_r.data);
    v_r.data = _mm512_mul_ps(v_r.data, beta2_r.data);
    struct AVX_Data avx_r0;
    avx_r0.data = _mm512_mul_ps(g_r.data, g_r.data);
    m_r.data = _mm512_fmadd_ps(g_r.data, beta1_minus_r.data, m_r.data);
    v_r.data = _mm512_fmadd_ps(avx_r0.data, beta2_minus_r.data, v_r.data);
    avx_r0.data = _mm512_sqrt_ps(v_r.data);
    avx_r0.data = _mm512_div_ps(m_r.data, _mm512_add_ps(avx_r0.data, epsilon_r.data));
    avx_r0.data = _mm512_fmadd_ps(var_r.data, decay_r.data, avx_r0.data);
    var_r.data = _mm512_fmadd_ps(avx_r0.data, lr_neg_r.data, var_r.data);
    _mm512_storeu_ps(var_ptr, var_r.data);
    _mm512_storeu_ps(m_ptr, m_r.data);
    _mm512_storeu_ps(v_ptr, v_r.data);

    gradient16_ptr += C16NUM;
    var_ptr += C16NUM;
    m_ptr += C16NUM;
    v_ptr += C16NUM;
  }
#endif
  return c1;
}
