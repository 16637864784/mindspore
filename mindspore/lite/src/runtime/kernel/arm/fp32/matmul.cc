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

#include "src/runtime/kernel/arm/fp32/matmul.h"
#include "nnacl/fp32/matmul.h"
#include "src/runtime/runtime_api.h"
#include "include/errorcode.h"

using mindspore::lite::RET_ERROR;
using mindspore::lite::RET_INPUT_TENSOR_ERROR;
using mindspore::lite::RET_MEMORY_FAILED;
using mindspore::lite::RET_OK;

namespace mindspore::kernel {
MatmulCPUKernel::~MatmulCPUKernel() { FreeTmpBuffer(); }

void MatmulCPUKernel::FreeTmpBuffer() {
  if (a_c12_ptr_ != nullptr) {
    free(a_c12_ptr_);
    a_c12_ptr_ = nullptr;
  }
  if (b_r8_ptr_ != nullptr) {
    free(b_r8_ptr_);
    b_r8_ptr_ = nullptr;
  }
  if (bias_ptr_ != nullptr) {
    free(bias_ptr_);
    bias_ptr_ = nullptr;
  }
}

int MatmulCPUKernel::MallocMatrixABuffer() {
  auto a_shape = in_tensors_[0]->shape();
  int batch = 1;
  for (size_t i = 0; i < a_shape.size() - 2; ++i) {
    batch *= a_shape[i];
  }
  params_->batch = batch;
  params_->row_ = params_->a_transpose_ ? a_shape[a_shape.size() - 1] : a_shape[a_shape.size() - 2];
  params_->deep_ = params_->a_transpose_ ? a_shape[a_shape.size() - 2] : a_shape[a_shape.size() - 1];
  params_->row_4_ = UP_ROUND(params_->row_, C4NUM);
  params_->row_12_ = UP_ROUND(params_->row_, C12NUM);

#ifdef ENABLE_ARM32
  a_c12_ptr_ = reinterpret_cast<float *>(malloc(params_->batch * params_->row_4_ * params_->deep_ * sizeof(float)));
  if (a_c12_ptr_ == nullptr) {
    FreeTmpBuffer();
    return RET_MEMORY_FAILED;
  }
  memset(a_c12_ptr_, 0, params_->row_4_ * params_->deep_ * sizeof(float));
#else
  a_c12_ptr_ = reinterpret_cast<float *>(malloc(params_->batch * params_->row_12_ * params_->deep_ * sizeof(float)));
  if (a_c12_ptr_ == nullptr) {
    FreeTmpBuffer();
    return RET_MEMORY_FAILED;
  }
  memset(a_c12_ptr_, 0, params_->row_12_ * params_->deep_ * sizeof(float));
#endif
  return RET_OK;
}

int MatmulCPUKernel::MallocMatrixBBuffer() {
  auto b_shape = in_tensors_[1]->shape();
  if (b_shape.empty()) {
    return RET_OK;
  }
  int batch = 1;
  for (size_t i = 0; i < b_shape.size() - 2; ++i) {
    batch *= b_shape[i];
  }
  params_->batch = batch;
  params_->col_ = params_->b_transpose_ ? b_shape[b_shape.size() - 2] : b_shape[b_shape.size() - 1];
  params_->col_8_ = UP_ROUND(params_->col_, 8);
  params_->deep_ = params_->b_transpose_ ? b_shape[b_shape.size() - 1] : b_shape[b_shape.size() - 2];

  b_r8_ptr_ = reinterpret_cast<float *>(malloc(params_->batch * params_->col_8_ * params_->deep_ * sizeof(float)));
  if (b_r8_ptr_ == nullptr) {
    FreeTmpBuffer();
    return RET_MEMORY_FAILED;
  }
  memset(b_r8_ptr_, 0, params_->col_8_ * params_->deep_ * sizeof(float));

  thread_count_ = MSMIN(thread_count_, UP_DIV(params_->col_8_, 8));
  thread_stride_ = UP_DIV(UP_DIV(params_->col_8_, 8), thread_count_);
  return RET_OK;
}

int MatmulCPUKernel::InitBias() {
  auto c_shape = out_tensors_[0]->shape();
  if (c_shape.empty()) {
    return RET_OK;
  }
  auto col_8 = UP_ROUND(c_shape[c_shape.size() - 1], 8);
  bias_ptr_ = reinterpret_cast<float *>(malloc(col_8 * sizeof(float)));
  if (bias_ptr_ == nullptr) {
    FreeTmpBuffer();
    return RET_MEMORY_FAILED;
  }
  memset(bias_ptr_, 0, col_8 * sizeof(float));
  if (in_tensors_.size() == 3) {
    auto bias_shape = in_tensors_[1]->shape();
    if (bias_shape[bias_shape.size() - 1] != c_shape[c_shape.size() - 1]) {
      MS_LOG(ERROR) << "The bias'dimension is not equal with colum";
      FreeTmpBuffer();
      return RET_INPUT_TENSOR_ERROR;
    }
    memcpy(bias_ptr_, in_tensors_[2]->data_c(), in_tensors_[2]->ElementsNum() * sizeof(float));
  }
  return RET_OK;
}

int MatmulCPUKernel::ReSize() {
  if (params_->a_const_ == false || params_->a_has_shape_ == false) {
    if (a_c12_ptr_ != nullptr) {
      free(a_c12_ptr_);
      a_c12_ptr_ = nullptr;
    }
    auto ret = MallocMatrixABuffer();
    if (ret != RET_OK) {
      MS_LOG(ERROR) << "Matmul fp32 malloc matrix a buffer failed";
      return RET_ERROR;
    }
  }
  if (params_->b_const_ == false || params_->b_has_shape_ == false) {
    if (b_r8_ptr_ != nullptr) {
      free(b_r8_ptr_);
      b_r8_ptr_ = nullptr;
    }
    auto ret = MallocMatrixBBuffer();
    if (ret != RET_OK) {
      MS_LOG(ERROR) << "Matmul fp32 malloc matrix b buffer failed";
      return RET_ERROR;
    }
  }
  if (bias_ptr_ != nullptr) {
    free(bias_ptr_);
    bias_ptr_ = nullptr;
  }
  auto ret = InitBias();
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "Matmul fp32 init bias failed";
    return RET_ERROR;
  }
  return RET_OK;
}

void MatmulCPUKernel::InitMatrixA(float *src_ptr, float *dst_ptr) {
  for (int i = 0; i < params_->batch; i++) {
    float *src = src_ptr + i * params_->deep_ * params_->row_;
#ifdef ENABLE_ARM32
    float *dst = dst_ptr + i * params_->deep_ * params_->row_4_;
    if (params_->a_transpose_) {
      RowMajor2Row4Major(src, dst, params_->deep_, params_->row_);
    } else {
      RowMajor2Col4Major(src, dst, params_->row_, params_->deep_);
    }
#else
    float *dst = dst_ptr + i * params_->deep_ * params_->row_12_;
    if (params_->a_transpose_) {
      RowMajor2Row12Major(src, dst, params_->deep_, params_->row_);
    } else {
      RowMajor2Col12Major(src, dst, params_->row_, params_->deep_);
    }
#endif
  }
  return;
}

void MatmulCPUKernel::InitMatrixB(float *src_ptr, float *dst_ptr) {
  for (int i = 0; i < params_->batch; i++) {
    float *src = src_ptr + i * params_->deep_ * params_->col_;
    float *dst = dst_ptr + i * params_->deep_ * params_->col_8_;
    if (params_->b_transpose_) {
      RowMajor2Col8Major(src, dst, params_->col_, params_->deep_);
    } else {
      RowMajor2Row8Major(src, dst, params_->deep_, params_->col_);
    }
  }
  return;
}

int MatmulCPUKernel::Init() {
  params_->a_has_shape_ = (in_tensors_[0]->shape().size() != 0);
  params_->b_has_shape_ = (in_tensors_[1]->shape().size() != 0);
  if (params_->a_has_shape_ == true) {
    auto ret = MallocMatrixABuffer();
    if (ret != RET_OK) {
      MS_LOG(ERROR) << "Matmul fp32 malloc matrix a buffer failed";
      return RET_ERROR;
    }
  }
  if (params_->b_has_shape_ == true) {
    auto ret = MallocMatrixBBuffer();
    if (ret != RET_OK) {
      MS_LOG(ERROR) << "Matmul fp32 malloc matrix b buffer failed";
      return RET_ERROR;
    }
  }

  params_->a_const_ = (in_tensors_[0]->data_c() != nullptr);
  params_->b_const_ = (in_tensors_[1]->data_c() != nullptr);
  if (params_->a_const_ == true) {
    InitMatrixA(reinterpret_cast<float *>(in_tensors_[0]->data_c()), a_c12_ptr_);
  }
  if (params_->b_const_ == true) {
    InitMatrixB(reinterpret_cast<float *>(in_tensors_[1]->data_c()), b_r8_ptr_);
  }
  if (!InferShapeDone()) {
    return RET_OK;
  }
  auto ret = InitBias();
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "Matmul fp32 init bias failed";
    return RET_ERROR;
  }
  return RET_OK;
}

int MatmulCPUKernel::RunImpl(int task_id) {
  int cur_oc = MSMIN(thread_stride_ * C8NUM, params_->col_ - task_id * thread_stride_ * C8NUM);
  if (cur_oc <= 0) {
    return RET_OK;
  }
  MatMulOpt(a_ptr_, b_ptr_ + task_id * thread_stride_ * C8NUM * params_->deep_,
            c_ptr_ + task_id * thread_stride_ * C8NUM, bias_ptr_ + task_id * thread_stride_ * C8NUM, ActType_No,
            params_->deep_, params_->row_, cur_oc, params_->col_, OutType_Nhwc);
  return RET_OK;
}

int MatmulFloatRun(void *cdata, int task_id) {
  auto op = reinterpret_cast<MatmulCPUKernel *>(cdata);
  auto error_code = op->RunImpl(task_id);
  if (error_code != RET_OK) {
    MS_LOG(ERROR) << "MatmulFp32Run error task_id[" << task_id << "] error_code[" << error_code << "]";
    return RET_ERROR;
  }
  return RET_OK;
}

int MatmulCPUKernel::Run() {
  auto prepare_ret = Prepare();
  if (prepare_ret != RET_OK) {
    MS_LOG(ERROR) << "Prepare fail!ret: " << prepare_ret;
    return prepare_ret;
  }
  auto a_src = reinterpret_cast<float *>(in_tensors_[0]->data_c());
  auto b_src = reinterpret_cast<float *>(in_tensors_[1]->data_c());
  auto c_src = reinterpret_cast<float *>(out_tensors_[0]->data_c());

  if (params_->a_const_ == false || is_train()) {
    InitMatrixA(a_src, a_c12_ptr_);
  }
  if (params_->b_const_ == false || is_train()) {
    InitMatrixB(b_src, b_r8_ptr_);
  }

  for (int i = 0; i < params_->batch; ++i) {
    a_ptr_ = a_c12_ptr_ + i * params_->row_12_ * params_->deep_;
    b_ptr_ = b_r8_ptr_ + i * params_->deep_ * params_->col_8_;
    c_ptr_ = c_src + i * params_->row_ * params_->col_;
    ParallelLaunch(this->context_->thread_pool_, MatmulFloatRun, this, thread_count_);
  }
  return RET_OK;
}

void MatmulCPUKernel::eval() {
  // Copy weights after training
  LiteKernel::eval();
  if (params_->a_const_ == true) {
    InitMatrixA(reinterpret_cast<float *>(in_tensors_[0]->MutableData()), a_c12_ptr_);
  }
  if (params_->b_const_ == true) {
    InitMatrixB(reinterpret_cast<float *>(in_tensors_[1]->MutableData()), b_r8_ptr_);
  }
}

}  // namespace mindspore::kernel
