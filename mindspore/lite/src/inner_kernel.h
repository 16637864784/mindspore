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

#ifndef MINDSPORE_LITE_SRC_INNER_KERNEL_H_
#define MINDSPORE_LITE_SRC_INNER_KERNEL_H_
#include <string>
#include <vector>
#include <memory>
#include <utility>
#include <algorithm>
#include "src/common/utils.h"
#include "src/common/log_util.h"
#include "nnacl/op_base.h"
#include "src/inner_context.h"
#include "src/tensor.h"
#include "include/errorcode.h"
#include "schema/model_generated.h"
#include "include/context.h"
#include "include/kernel.h"

namespace mindspore::kernel {
class InnerKernel : public Kernel {
 public:
  InnerKernel() = default;

  InnerKernel(OpParameter *parameter, std::vector<lite::Tensor *> in_tensors, std::vector<lite::Tensor *> out_tensors,
              const lite::Context *ctx)
      : op_parameter_(parameter), in_tensors_(std::move(in_tensors)), out_tensors_(std::move(out_tensors)) {
    context_ = ctx;
    if (parameter != nullptr && parameter->thread_num_ == 0) {
      if (ctx != nullptr) {
        op_parameter_->thread_num_ = ctx->thread_num_;
      } else {
        op_parameter_->thread_num_ = 1;
      }
    }
  }

  virtual ~InnerKernel() {
    if (op_parameter_ != nullptr) {
      free(op_parameter_);
      op_parameter_ = nullptr;
    }
  }

  int Execute() override {
    auto ret = PreProcess();
    if (lite::RET_OK != ret) {
      MS_LOG(ERROR) << "run kernel PreProcess failed, name: " << this->name();
      return ret;
    }

    // Support ZeroShape
    size_t zero_shape_num = 0;
    for (auto tensor : this->out_tensors()) {
      for (size_t i = 0; i < tensor->shape().size(); i++) {
        if (tensor->shape()[i] == 0) {
          zero_shape_num++;
          break;
        }
      }
    }

    if (zero_shape_num != this->out_tensors().size()) {
      auto ret = Run();
      if (lite::RET_OK != ret) {
        MS_LOG(ERROR) << "run kernel failed, name: " << this->name();
        return ret;
      }
    }

    ret = PostProcess();
    if (lite::RET_OK != ret) {
      MS_LOG(ERROR) << "run kernel PreProcess failed, name: " << this->name();
      return ret;
    }
    return lite::RET_OK;
  }

  // called while compiling graph
  int Prepare() override { return mindspore::lite::RET_OK; }
  virtual int Run() { return mindspore::lite::RET_ERROR; }
  int ReSize() override { return mindspore::lite::RET_ERROR; }

  // called before Run
  virtual int PreProcess();
  // called after Run
  virtual int PostProcess() {
    for (auto *output : this->out_tensors()) {
      MS_ASSERT(output != nullptr);
      output->ResetRefCount();
    }

    return FreeInWorkTensor();
  }

  virtual int FreeInWorkTensor() const {
    for (auto &in_tensor : this->in_tensors()) {
      MS_ASSERT(in_tensor != nullptr);
      if (in_tensor->root_tensor() == in_tensor) {
        continue;
      }
      in_tensor->DecRefCount();
    }
    return lite::RET_OK;
  }

  virtual int Init() { return mindspore::lite::RET_OK; }

  OpParameter *op_parameter() const { return op_parameter_; }

  bool InferShapeDone() const {
    auto shape = out_tensors_.front()->shape();
    if (std::find(shape.begin(), shape.end(), -1) != shape.end()) {
      return false;
    }
    return true;
  }

  schema::PrimitiveType type() const override {
    return (this->op_parameter_ != nullptr) ? schema::PrimitiveType(this->op_parameter_->type_)
                                            : schema::PrimitiveType_NONE;
  }

  void set_inputs(const std::vector<mindspore::tensor::MSTensor *> &in_tensors) override {
    this->in_tensors_.resize(in_tensors.size());
    (void)std::transform(in_tensors.begin(), in_tensors.end(), in_tensors_.begin(),
                         [](mindspore::tensor::MSTensor *tensor) { return static_cast<lite::Tensor *>(tensor); });
  }

  void set_outputs(const std::vector<mindspore::tensor::MSTensor *> &out_tensors) override {
    this->out_tensors_.resize(out_tensors.size());
    (void)std::transform(out_tensors.begin(), out_tensors.end(), out_tensors_.begin(),
                         [](mindspore::tensor::MSTensor *tensor) { return static_cast<lite::Tensor *>(tensor); });
  }

  const std::vector<mindspore::tensor::MSTensor *> &inputs() override {
    inputs_.assign(in_tensors_.begin(), in_tensors_.end());
    return inputs_;
  }

  const std::vector<mindspore::tensor::MSTensor *> &outputs() override {
    outputs_.assign(out_tensors_.begin(), out_tensors_.end());
    return outputs_;
  }

  void set_in_tensors(const std::vector<lite::Tensor *> &in_tensors) { this->in_tensors_ = in_tensors; }

  virtual void set_in_tensor(lite::Tensor *in_tensor, int index) {
    MS_ASSERT(index < in_tensors_.size());
    this->in_tensors_[index] = in_tensor;
  }

  void set_out_tensors(const std::vector<lite::Tensor *> &out_tensors) { this->out_tensors_ = out_tensors; }

  virtual void set_out_tensor(lite::Tensor *out_tensor, int index) {
    MS_ASSERT(index < out_tensors_.size());
    this->out_tensors_[index] = out_tensor;
  }

  const std::vector<lite::Tensor *> &in_tensors() const { return in_tensors_; }

  const std::vector<lite::Tensor *> &out_tensors() const { return out_tensors_; }

  virtual int Train() {
    this->train_mode_ = true;
    return mindspore::lite::RET_OK;
  }

  virtual bool IsTrain() const { return this->train_mode_; }

  virtual int Eval() {
    this->train_mode_ = false;
    return mindspore::lite::RET_OK;
  }

  virtual bool IsEval() const { return !this->train_mode_; }

  virtual void set_trainable(bool trainable = true) { this->trainable_ = trainable; }

  virtual bool is_trainable() const { return this->trainable_; }

  TypeId registry_data_type(void) { return registry_data_type_; }

  void set_registry_data_type(TypeId data_type) { registry_data_type_ = data_type; }

  void set_workspace_size(size_t value) { workspace_size_ = value; }
  size_t workspace_size() { return workspace_size_; }
  static void AllocWorkspace(size_t size);
  static void FreeWorkspace();
  void *workspace() { return workspace_; }

 protected:
  OpParameter *op_parameter_ = nullptr;
  // tensor will free in ~lite_session()
  std::vector<lite::Tensor *> in_tensors_;
  std::vector<lite::Tensor *> out_tensors_;
  bool train_mode_ = false;
  bool trainable_ = false;  // parameters of this Kernel are trained in Train Session
  TypeId registry_data_type_ = kTypeUnknown;
  size_t workspace_size_ = 0;
  static void *workspace_;
};
}  // namespace mindspore::kernel

#endif  // MINDSPORE_LITE_SRC_INNER_KERNEL_H_
