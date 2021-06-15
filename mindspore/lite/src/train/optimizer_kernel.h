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
#ifndef MINDSPORE_LITE_SRC_TRAIN_OPTIMIZER_KERNEL_H_
#define MINDSPORE_LITE_SRC_TRAIN_OPTIMIZER_KERNEL_H_
#include <vector>
#include <cmath>
#include <cfloat>
#include "src/lite_kernel.h"
#include "include/errorcode.h"
using mindspore::lite::RET_ERROR;
using mindspore::lite::RET_OK;
using mindspore::lite::RET_OUT_OF_TENSOR_RANGE;

static __attribute__((always_inline)) inline bool MS_ISNAN(float var) {
  volatile float d = var;
  return d != d;
}

namespace mindspore::kernel {

class OptimizerKernel : public InnerKernel {
 public:
  OptimizerKernel() = default;
  OptimizerKernel(OpParameter *parameter, const std::vector<lite::Tensor *> &inputs,
                  const std::vector<lite::Tensor *> &outputs, const lite::InnerContext *ctx, int lr_idx, int grad_idx)
      : InnerKernel(parameter, inputs, outputs, ctx), lr_idx_(lr_idx), grad_idx_(grad_idx) {}
  ~OptimizerKernel() = default;

  enum class WeightUpdateMode { NORMAL, VIRTUAL_BATCH };
  WeightUpdateMode get_optimizer_mode() { return weight_update_mod_; }

  int Init() override {
    default_lr_ = reinterpret_cast<float *>(in_tensors_.at(lr_idx_)->MutableData())[0];
    lr_ = default_lr_;
    return RET_OK;
  }

  int SetLearningRate(float lr) {
    lr_ = lr;
    return RET_OK;
  }

  float GetLearningRate() { return lr_; }

  int RestoreDefaultLearningRate() {
    SetLearningRate(default_lr_);
    return RET_OK;
  }

  int SetOptimizerMode(WeightUpdateMode mod) {
    if (mod == WeightUpdateMode::VIRTUAL_BATCH) {
      if (grad_sum_ != nullptr) {
        context_->allocator->Free(grad_sum_);
        grad_sum_ = nullptr;
      }
      size_t size = in_tensors_.at(grad_idx_)->Size();
      size_t elem_num = in_tensors_.at(grad_idx_)->ElementsNum();
      grad_sum_ = reinterpret_cast<float *>(context_->allocator->Malloc(size));
      if (grad_sum_ == nullptr) {
        MS_LOG(ERROR) << "failed to malloc grad sum tensor, size=" << size;
        return RET_ERROR;
      }
      valid_grad_sum_ = false;
      std::fill(grad_sum_, grad_sum_ + elem_num, 0);
      weight_update_mod_ = WeightUpdateMode::VIRTUAL_BATCH;
    } else {
      if (grad_sum_ != nullptr) {
        OptimizerStep();
        context_->allocator->Free(grad_sum_);
        grad_sum_ = nullptr;
      }
    }
    return RET_OK;
  }

  int ExecuteVirtualBatch(int task_id) {
    auto gradient = reinterpret_cast<float *>(in_tensors_.at(grad_idx_)->MutableData());
    int length = in_tensors_.at(grad_idx_)->ElementsNum();

    int stride = UP_DIV(length, context_->thread_num_);
    int count = MSMIN(stride, length - stride * task_id);
    int start = stride * task_id;
    int end = start + count;
    for (int i = start; i < end; ++i) {
      grad_sum_[i] += gradient[i];
    }
    valid_grad_sum_ = true;
    return RET_OK;
  }

  virtual int OptimizerStep() {
    valid_grad_sum_ = false;
    return RET_OK;
  }

  int Eval() override {
    OptimizerStep();
    return InnerKernel::Eval();
  }

  int PreProcess() override {
    auto ret = InnerKernel::PreProcess();
    if (ret != RET_OK) {
      return ret;
    }

    auto ctx = static_cast<const lite::InnerContext *>(this->context_);
    if (ctx->IsCpuFloat16Enabled()) {
      auto t = in_tensors_.at(grad_idx_);
      auto gradient = reinterpret_cast<float *>(t->data_c());
      int length = in_tensors_.at(grad_idx_)->ElementsNum();

      for (int i = 0; i < length; ++i) {
        if (MS_ISNAN(gradient[i]) || std::isinf(gradient[i])) {
          MS_LOG(INFO) << "optimizer grad is nan or inf";
          return RET_OUT_OF_TENSOR_RANGE;
        }
      }

      auto is_scale = t->IsScale();
      auto scale = t->get_scale();
      if (is_scale) {
        t->set_scale(1.0f / scale);
        for (int i = 0; i < length; ++i) {
          gradient[i] *= (1.0f / scale);
        }
      }
    }
    return RET_OK;
  }

 protected:
  float default_lr_ = 0.0f;
  float lr_ = 0.0f;
  int lr_idx_ = 0;
  int grad_idx_ = 0;
  float *grad_sum_ = nullptr;
  bool valid_grad_sum_ = false;

 private:
  WeightUpdateMode weight_update_mod_ = WeightUpdateMode::NORMAL;
};

}  // namespace mindspore::kernel
#endif  // MINDSPORE_LITE_SRC_TRAIN_OPTIMIZER_KERNEL_H_
