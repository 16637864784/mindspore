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

#include "src/train/train_session.h"
#include <sys/stat.h>
#include <algorithm>
#include <utility>
#include <vector>
#include <iostream>
#include <fstream>
#include <memory>
#include "include/errorcode.h"
#include "src/common/utils.h"
#include "src/tensor.h"
#include "src/lite_model.h"
#include "src/train/loss_kernel.h"
#include "src/train/optimizer_kernel.h"
#include "src/sub_graph_kernel.h"
#include "src/train/train_populate_parameter.h"
#include "src/train/train_populate_parameter_v0.h"
#include "src/runtime/runtime_api.h"
#include "src/executor.h"
#include "src/kernel_registry.h"
#include "src/runtime/kernel/arm/fp32_grad/convolution.h"
#include "src/runtime/kernel/arm/fp32/batchnorm_fp32.h"
#include "src/common/tensor_util.h"
#include "src/train/train_utils.h"
#include "src/train/train_export.h"

namespace mindspore {
namespace lite {

TrainSession::TrainSession() {
  is_train_session_ = true;
#ifdef ENABLE_V0
  if (VersionManager::GetInstance()->CheckV0Schema()) {
    kernel::PopulateTrainV0Parameters();
  }
#endif
  if (!VersionManager::GetInstance()->CheckV0Schema()) {
    kernel::PopulateTrainParameters();
  }
}

std::vector<CreatorOp> TrainSession::ReplaceOps() {
  const std::vector<CreatorOp> replace = {
    // currently no ops are Hijacked by TrainSession
  };
  mindspore::lite::KernelRegistry *reg = mindspore::lite::KernelRegistry::GetInstance();
  std::vector<CreatorOp> results;
  for (auto v : replace) {
    const CreatorOp cl = make_tuple(std::get<0>(v), reg->GetCreator(std::get<0>(v)));
    results.push_back(cl);
    reg->RegKernel(std::get<0>(v), std::get<1>(v));
  }
  return results;
}

void TrainSession::RestoreOps(const std::vector<CreatorOp> &restore) {
  mindspore::lite::KernelRegistry *reg = mindspore::lite::KernelRegistry::GetInstance();
  for (auto v : restore) {
    reg->RegKernel(std::get<0>(v), std::get<1>(v));
  }
}

void TrainSession::AllocWorkSpace() {
  size_t workspace_size = 0;
  for (auto kernel : this->train_kernels_) {
    if (workspace_size < static_cast<kernel::InnerKernel *>(kernel->kernel())->workspace_size()) {
      workspace_size = static_cast<kernel::InnerKernel *>(kernel->kernel())->workspace_size();
    }
  }
  mindspore::kernel::InnerKernel::AllocWorkspace(workspace_size);
}

int TrainSession::CompileGraph(lite::Model *model) { return lite::RET_ERROR; }

int TrainSession::CompileTrainGraph(mindspore::lite::Model *model) {
  model_ = model;

  auto restore = ReplaceOps();
  auto ret = lite::LiteSession::CompileGraph(model);
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "failed to compile train model";
    return RET_ERROR;
  }
  orig_output_node_map_ = output_node_map_;
  orig_output_tensor_map_ = output_tensor_map_;
  orig_output_tensor_names_ = output_tensor_names_;
  for (auto inTensor : inputs_) inTensor->MutableData();
  RestoreOps(restore);
  CompileTrainKernels();      // Prepare a list of train kernels
  CompileOptimizedKernels();  // Prepare a list of kernels which are optimized (weight update step)
  CompileTrainOutputs();      // prepare outputs in train mode
  CompileEvalOutputs();       // prepare outputs in eval mode
  CompileInferenceKernels();  // Prepare a list of eval kernels
  AllocWorkSpace();

  return RET_OK;
}

TrainSession::~TrainSession() {
  mindspore::kernel::InnerKernel::FreeWorkspace();
  if (model_ != nullptr) {
    delete model_;
    model_ = nullptr;
  }
}

int TrainSession::RunGraph(const KernelCallBack &before, const KernelCallBack &after) {
  this->outputs_.clear();

  // build out tensor
  for (auto ms_tensors : output_node_map_) {
    for (auto ms_tensor : ms_tensors.second) {
      this->outputs_.push_back((static_cast<lite::Tensor *>(ms_tensor)));
    }
  }

  if (this->context_ == nullptr) {
    MS_LOG(ERROR) << "context is null";
    return lite::RET_NULL_PTR;
  }
  auto run_kernel = (train_mode_) ? train_kernels_ : inference_kernels_;

  auto ret = CheckTensorsInvalid(inputs_);
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "CheckInputs failed";
    return ret;
  }

  for (auto *kernel : run_kernel) {
    MS_ASSERT(nullptr != kernel);
    ret = kernel->Execute(before, after);
    if (RET_OK != ret) {
      MS_LOG(ERROR) << "run kernel failed, name: " << kernel->name();
      return ret;
    }
  }

  if (train_mode_ && virtual_batch_multiplier_) {
    virtual_batch_idx_++;
    if (virtual_batch_idx_ >= virtual_batch_multiplier_) {
      virtual_batch_idx_ = 0;
      ret = OptimizerStep();
      if (ret != RET_OK) {
        MS_LOG(ERROR) << "failed to optimize model weights";
        return ret;
      }
    }
  }
  return RET_OK;
}

int TrainSession::Train() {
  // shift kernels to train mode
  train_mode_ = true;
  virtual_batch_idx_ = 0;
  for (auto kernel : this->train_kernels_) {
    MS_ASSERT(nullptr != kernel);
    auto ret = kernel->Train();
    if (ret != RET_OK) {
      MS_LOG(ERROR) << kernel->name() << " failed to set train mode";
      return RET_ERROR;
    }
  }
  // set train outputs
  output_node_map_ = train_output_node_map_;
  output_tensor_map_ = train_output_tensor_map_;
  output_tensor_names_ = train_output_tensor_names_;
  return RET_OK;
}

int TrainSession::Eval() {
  // shift kernels to eval mode
  train_mode_ = false;
  virtual_batch_idx_ = 0;
  for (auto kernel : this->train_kernels_) {
    MS_ASSERT(kernel != nullptr);
    auto ret = kernel->Eval();
    if (ret != RET_OK) {
      MS_LOG(ERROR) << kernel->name() << " failed to set eval mode";
      return RET_ERROR;
    }
  }
  // set eval outputs
  output_node_map_ = eval_output_node_map_;
  output_tensor_map_ = eval_output_tensor_map_;
  output_tensor_names_ = eval_output_tensor_names_;
  return RET_OK;
}

void TrainSession::CompileEvalOutputs() {
  eval_output_node_map_.clear();
  eval_output_tensor_map_.clear();
  eval_output_tensor_names_.clear();
  for (auto kernel : this->train_kernels_) {
    if (IsLossKernel(kernel) && !(IsGradKernel(kernel))) {
      for (auto in_kernel : kernel->in_kernels()) {
        if (IsLossKernel(in_kernel) || IsGradKernel(in_kernel)) continue;
        // insert if not already in
        if (eval_output_node_map_.find(in_kernel->name()) == eval_output_node_map_.end()) {
          auto *ms_tensor = in_kernel->out_tensors().at(0);
          if (ms_tensor != nullptr) {
            ms_tensor->set_init_ref_count(ms_tensor->init_ref_count() + 1);
            eval_output_node_map_[in_kernel->name()].emplace_back(ms_tensor);
            auto index = TSFindTensor(tensors_, ms_tensor);
            if (index != tensors_.size()) {
              eval_output_tensor_map_.insert(std::make_pair(std::to_string(index), ms_tensor));
              if (!ms_tensor->tensor_name().empty()) {
                eval_output_tensor_names_.emplace_back(ms_tensor->tensor_name());
              } else {
                eval_output_tensor_names_.emplace_back(std::to_string(index));
              }
            }
          }
        }
      }
    }
  }
  if (eval_output_node_map_.size() == 0) eval_output_node_map_ = orig_output_node_map_;
  if (eval_output_tensor_map_.size() == 0) eval_output_tensor_map_ = orig_output_tensor_map_;
  if (eval_output_tensor_names_.size() == 0) eval_output_tensor_names_ = orig_output_tensor_names_;
}

void TrainSession::CompileTrainOutputs() {
  train_output_node_map_.clear();
  train_output_tensor_map_.clear();
  train_output_tensor_names_.clear();
  for (auto kernel : this->train_kernels_) {
    if (orig_output_node_map_.find(kernel->name()) == orig_output_node_map_.end()) continue;
    // Mask out optimizer out tensors
    if (IsMaskOutput(kernel)) continue;
    // insert if not already in
    if (train_output_node_map_.find(kernel->name()) == train_output_node_map_.end()) {
      auto *ms_tensor = kernel->out_tensors().at(0);
      if (ms_tensor != nullptr) {
        train_output_node_map_[kernel->name()].emplace_back(ms_tensor);
        auto index = TSFindTensor(tensors_, ms_tensor);
        if (index != tensors_.size()) {
          train_output_tensor_map_.insert(std::make_pair(std::to_string(index), ms_tensor));
          if (!ms_tensor->tensor_name().empty()) {
            train_output_tensor_names_.emplace_back(ms_tensor->tensor_name());
          } else {
            train_output_tensor_names_.emplace_back(std::to_string(index));
          }
        }
      }
    }
  }
  if (train_output_node_map_.size() == 0) train_output_node_map_ = orig_output_node_map_;
  if (train_output_tensor_map_.size() == 0) train_output_tensor_map_ = orig_output_tensor_map_;
  if (train_output_tensor_names_.size() == 0) train_output_tensor_names_ = orig_output_tensor_names_;
}

void TrainSession::BuildInferenceKernelsRecursive(kernel::LiteKernel *kernel, std::vector<kernel::LiteKernel *> *v) {
  if (std::find(v->begin(), v->end(), kernel) == v->end()) {  // kernel is not already in vector
    for (auto in_node : kernel->in_kernels()) {
      BuildInferenceKernelsRecursive(in_node, v);
    }
    if (!IsLossKernel(kernel)) v->push_back(kernel);
  }
}

void TrainSession::CompileTrainKernels() {
  train_kernels_.clear();
  for (auto ori_kernel : kernels_) {
    if (ori_kernel->subgraph_type() == kernel::kNotSubGraph) {
      train_kernels_.push_back(ori_kernel);
    } else {
      auto sub_graph = reinterpret_cast<kernel::SubGraphKernel *>(ori_kernel);
      for (auto kernel : sub_graph->nodes()) {
        train_kernels_.push_back(kernel);
      }
    }
  }
}

void TrainSession::CompileInferenceKernels() {
  inference_kernels_.clear();
  for (auto item : eval_output_node_map_) {
    std::string kernel_name = item.first;
    auto kernel = TSFindKernel(train_kernels_, kernel_name);
    BuildInferenceKernelsRecursive(kernel, &inference_kernels_);
  }
  if (inference_kernels_.size() == 0) {
    inference_kernels_ = this->train_kernels_;
  }
}

void TrainSession::CompileOptimizedKernels() {
  std::vector<lite::Tensor *> out_tensor;
  for (auto kernel : this->train_kernels_) {
    if (IsOptimizer(kernel)) {
      std::copy(kernel->in_tensors().begin(), kernel->in_tensors().end(), std::back_inserter(out_tensor));
    }
  }

  for (auto kernel : this->train_kernels_) {
    if (!IsOptimizer(kernel)) {
      for (auto it : kernel->in_tensors()) {
        if (std::find(out_tensor.begin(), out_tensor.end(), it) != out_tensor.end()) {
          kernel->set_trainable(true);
          break;
        }
      }
    }
  }
}

int TrainSession::SetLearningRate(float learning_rate) {
  if (learning_rate < 0.0f) {
    MS_LOG(ERROR) << "learning rate should more than 0";
    return RET_ERROR;
  }
  for (auto kernel : this->train_kernels_) {
    if (IsOptimizer(kernel)) {
      auto optimizer = reinterpret_cast<kernel::OptimizerKernel *>(kernel);
      auto ret = optimizer->SetLearningRate(learning_rate);
      if (ret != RET_OK) {
        MS_LOG(ERROR) << kernel->name() << " failed to set learning rate";
        return RET_ERROR;
      }
    }
  }
  return RET_OK;
}

float TrainSession::GetLearningRate() {
  for (auto kernel : this->train_kernels_) {
    if (IsOptimizer(kernel)) {
      auto optimizer = reinterpret_cast<kernel::OptimizerKernel *>(kernel);
      return optimizer->GetLearningRate();
    }
  }
  return 0.0;
}

int TrainSession::AdminSetupVirtualBatch(int virtual_batch_multiplier, float lr, float momentum) {
  auto mod = (virtual_batch_multiplier <= 1) ? kernel::OptimizerKernel::WeightUpdateMode::NORMAL
                                             : kernel::OptimizerKernel::WeightUpdateMode::VIRTUAL_BATCH;
  virtual_batch_multiplier_ = (virtual_batch_multiplier <= 1) ? 0 : virtual_batch_multiplier;
  virtual_batch_idx_ = 0;

  for (auto kernel : this->train_kernels_) {
    if (IsOptimizer(kernel)) {
      auto optimizer = reinterpret_cast<kernel::OptimizerKernel *>(kernel);
      auto ret = optimizer->SetOptimizerMode(mod);
      if (ret != RET_OK) {
        MS_LOG(ERROR) << kernel->name() << " failed to set optimizer mode";
        return RET_ERROR;
      }
      if (mod == kernel::OptimizerKernel::WeightUpdateMode::VIRTUAL_BATCH) {
        lr = (lr < 0.0f) ? (optimizer->GetLearningRate() / static_cast<float>(virtual_batch_multiplier_)) : lr;
        ret = optimizer->SetLearningRate(lr);
      } else {
        ret = optimizer->RestoreDefaultLearningRate();
      }
      if (ret != RET_OK) {
        MS_LOG(ERROR) << kernel->name() << " failed to set learning rate";
        return RET_ERROR;
      }
    }

    if (IsBN(kernel) && kernel->is_trainable()) {
      auto batchnorm = reinterpret_cast<kernel::BatchnormCPUKernel *>(kernel);
      auto ret = RET_OK;
      if (mod == kernel::OptimizerKernel::WeightUpdateMode::VIRTUAL_BATCH) {
        momentum = (momentum < 0.0f) ? (batchnorm->get_momentum() / virtual_batch_multiplier_) : momentum;
        ret = batchnorm->set_momentum(momentum);
      } else {
        ret = batchnorm->RestoreDefaultMomentum();
      }
      if (ret != RET_OK) {
        MS_LOG(ERROR) << kernel->name() << " failed to set momentum";
        return RET_ERROR;
      }
    }
  }
  return RET_OK;
}
int TrainSession::SetupVirtualBatch(int virtual_batch_multiplier, float lr, float momentum) {
  int tmp = (virtual_batch_multiplier <= 1) ? 0 : virtual_batch_multiplier;
  if (tmp != 0 && virtual_batch_multiplier_ != 0) {
    AdminSetupVirtualBatch(0, lr, momentum);
  }
  return AdminSetupVirtualBatch(virtual_batch_multiplier, lr, momentum);
}

int TrainSession::OptimizerStep() {
  for (auto kernel : this->train_kernels_) {
    if (IsOptimizer(kernel)) {
      auto optimizer = reinterpret_cast<kernel::OptimizerKernel *>(kernel);
      auto ret = optimizer->OptimizerStep();
      if (ret != RET_OK) {
        MS_LOG(ERROR) << kernel->name() << " failed to do optimize step";
        return RET_ERROR;
      }
    }
  }
  return RET_OK;
}

bool TrainSession::IsLossKernel(const kernel::LiteKernel *kernel) const {
  return (kernel->type() == schema::PrimitiveType_SoftmaxCrossEntropyWithLogits ||
          kernel->type() == schema::PrimitiveType_SparseSoftmaxCrossEntropyWithLogits ||
          kernel->type() == schema::PrimitiveType_SmoothL1Loss ||
          kernel->type() == schema::PrimitiveType_SmoothL1LossGrad ||
          kernel->type() == schema::PrimitiveType_SigmoidCrossEntropyWithLogits ||
          kernel->type() == schema::PrimitiveType_SigmoidCrossEntropyWithLogitsGrad) ||
         kernel->name().find(get_loss_name()) != std::string::npos;
}

bool TrainSession::IsGradKernel(const kernel::LiteKernel *kernel) const {
  return kernel->name().find("Gradients") != std::string::npos;
}

bool TrainSession::IsOptimizer(kernel::LiteKernel *kernel) const {
  return ((kernel->type() == schema::PrimitiveType_Adam) || (kernel->type() == schema::PrimitiveType_SGD) ||
          (kernel->type() == schema::PrimitiveType_ApplyMomentum));
}

bool TrainSession::IsMaskOutput(kernel::LiteKernel *kernel) const {
  return (IsOptimizer(kernel) || (kernel->type() == schema::PrimitiveType_Assign));
}

bool TrainSession::IsBN(kernel::LiteKernel *kernel) const {
  return ((kernel->type() == schema::PrimitiveType_BatchNorm) ||
          (kernel->type() == schema::PrimitiveType_FusedBatchNorm));
}

int TrainSession::SetLossName(std::string loss_name) {
  session::TrainSession::SetLossName(loss_name);
  CompileEvalOutputs();
  CompileInferenceKernels();
  if (IsEval()) {
    output_node_map_ = eval_output_node_map_;
    output_tensor_map_ = eval_output_tensor_map_;
    output_tensor_names_ = eval_output_tensor_names_;
  }
  return RET_OK;
}

int TrainSession::ExportInference(std::string file_name) {
  bool orig_train_state = IsTrain();
  Eval();
  TrainExport texport(file_name);
  int status = texport.ExportInit(model_->name_, model_->version_);
  if (status != RET_OK) {
    MS_LOG(ERROR) << "cannot init export";
    return status;
  }
  status = texport.ExportNet(inference_kernels_, tensors_, GetOutputTensorNames(), model_);
  if (status != RET_OK) {
    MS_LOG(ERROR) << "cannot export Network";
    return status;
  }
  status = texport.SaveToFile();
  if (status != RET_OK) {
    MS_LOG(ERROR) << "failed to save to " << file_name;
    return status;
  }
  if (orig_train_state) Train();
  return status;
}

}  // namespace lite

session::TrainSession *session::TrainSession::CreateSession(mindspore::lite::Model *model, lite::Context *context,
                                                            bool train_mode) {
  auto session = new (std::nothrow) lite::TrainSession();
  if (session == nullptr) {
    delete model;
    MS_LOG(ERROR) << "create session failed";
    return nullptr;
  }
  auto ret = session->Init(context);
  if (ret != mindspore::lite::RET_OK) {
    MS_LOG(ERROR) << "init session failed";
    delete session;
    delete model;
    return nullptr;
  }

  ret = session->CompileTrainGraph(model);
  if (ret != mindspore::lite::RET_OK) {
    MS_LOG(ERROR) << "Compiling Train Graph session failed";
    delete session;
    return nullptr;
  }

  if (train_mode) {
    ret = session->Train();
  } else {
    ret = session->Eval();
  }
  if (ret != mindspore::lite::RET_OK) {
    MS_LOG(ERROR) << "Could not switch to Train Modei " << train_mode;
    delete session;
    return nullptr;
  }

  return session;
}
}  // namespace mindspore
