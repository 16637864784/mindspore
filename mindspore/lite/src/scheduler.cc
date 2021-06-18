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

#include "src/scheduler.h"
#include <map>
#include <queue>
#include <string>
#include <vector>
#include <algorithm>
#include "src/tensorlist.h"
#include "include/errorcode.h"
#include "src/common/graph_util.h"
#include "src/common/utils.h"
#include "src/kernel_registry.h"
#include "include/registry/register_kernel.h"
#include "src/lite_kernel_util.h"
#include "src/sub_graph_kernel.h"
#include "src/ops/populate/populate_register.h"
#include "src/common/version_manager.h"
#include "src/common/prim_util.h"
#include "src/runtime/infer_manager.h"
#include "src/sub_graph_split.h"
#include "src/weight_decoder.h"
#if GPU_OPENCL
#include "src/runtime/kernel/opencl/opencl_subgraph.h"
#include "src/runtime/gpu/opencl/opencl_runtime.h"
#endif
#if SUPPORT_NPU
#include "src/runtime/agent/npu/subgraph_npu_kernel.h"
#include "src/runtime/agent/npu/npu_manager.h"
#include "src/runtime/agent/npu/optimizer/npu_pass_manager.h"
#include "src/runtime/agent/npu/optimizer/npu_transform_pass.h"
#include "src/runtime/agent/npu/optimizer/npu_fusion_pass.h"
#include "src/runtime/agent/npu/optimizer/npu_insert_transform_pass.h"
#endif
#if defined(ENABLE_ARM) && defined(ENABLE_FP16)
#include "src/runtime/kernel/arm/fp16/fp16_op_handler.h"
#endif
#include "include/registry/kernel_interface.h"

namespace mindspore::lite {
namespace {
constexpr int kMainSubGraphIndex = 0;
kernel::SubGraphKernel *CreateCustomSubGraph(std::vector<kernel::LiteKernel *> &&input_kernels,
                                             std::vector<kernel::LiteKernel *> &&output_kernels,
                                             const std::vector<kernel::LiteKernel *> &kernels, kernel::Kernel *kernel) {
  auto sub_kernel = new (std::nothrow) kernel::CustomSubGraph(input_kernels, output_kernels, kernels, kernel);
  if (sub_kernel == nullptr) {
    MS_LOG(ERROR) << "create custom subgraph failed!";
    delete kernel;
    return nullptr;
  }
  return sub_kernel;
}
}  // namespace

int Scheduler::Schedule(std::vector<kernel::LiteKernel *> *dst_kernels) {
  if (dst_kernels == nullptr) {
    return RET_ERROR;
  }
  if (src_model_ == nullptr) {
    MS_LOG(ERROR) << "Input model is nullptr";
    return RET_PARAM_INVALID;
  }
  if (src_model_->sub_graphs_.empty()) {
    MS_LOG(ERROR) << "Model should have a subgraph at least";
    return RET_PARAM_INVALID;
  }

  this->graph_output_node_indexes_ = GetGraphOutputNodes(src_model_);

  auto ret = InferSubGraphShape(kMainSubGraphIndex);
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "op infer shape failed.";
    return ret;
  }

  if (context_->enable_parallel_) {
    auto search_sub_graph =
      SearchSubGraph(context_, src_model_, src_tensors_, &op_parameters_, &graph_output_node_indexes_);
    search_sub_graph.SubGraphSplit();
  }

  ret = ScheduleSubGraphToKernels(kMainSubGraphIndex, dst_kernels, nullptr, nullptr);
  op_parameters_.clear();
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "Schedule main subgraph to kernels failed.";
    return ret;
  }
  if (delegate_ != nullptr) {
    ret = ReplaceDelegateKernels(dst_kernels);
    if (ret != RET_OK) {
      MS_LOG(ERROR) << "Repalce delegate kernels failed.";
      return ret;
    }
  }
  FindAllInoutKernels(*dst_kernels);

  ret = RunPass(dst_kernels);
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "Schedule run pass failed.";
    return ret;
  }
  auto src_kernel = *dst_kernels;
  dst_kernels->clear();
  std::map<const kernel::LiteKernel *, bool> is_kernel_finish;
  ret = ConstructSubGraphs(src_kernel, dst_kernels, &is_kernel_finish);
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "ConstructSubGraphs failed.";
    return ret;
  }
  MS_LOG(DEBUG) << "schedule kernels success.";
  return RET_OK;
}

int Scheduler::ReplaceDelegateKernels(std::vector<kernel::LiteKernel *> *dst_kernels) {
  std::vector<kernel::Kernel *> kernels;
  for (size_t i = 0; i < dst_kernels->size(); i++) {
    kernels.push_back((*dst_kernels)[i]->kernel());
  }
  DelegateModel *model = new (std::nothrow) DelegateModel(&kernels, primitives_);
  if (model == nullptr) {
    MS_LOG(ERROR) << "New delegate model failed.";
    return RET_NULL_PTR;
  }
  auto ret = delegate_->Build(model);
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "Delegate prepare kernels failed.";
    return ret;
  }

  auto src_kernels = *dst_kernels;
  dst_kernels->clear();
  std::map<const kernel::LiteKernel *, bool> delegate_support;
  for (auto kernel : src_kernels) {
    delegate_support[kernel] = true;
  }
  for (auto kernel : kernels) {
    size_t index = 0;
    for (; index < src_kernels.size(); index++) {
      if (kernel == src_kernels[index]->kernel()) {
        // Kernels that the delegate does not support keep the original backend
        dst_kernels->push_back(src_kernels[index]);
        delegate_support[src_kernels[index]] = false;
        break;
      }
    }
    if (index == src_kernels.size()) {
      // New liteKernel to save delegate subgraph
      std::shared_ptr<kernel::Kernel> shared_kernel(kernel);
      auto lite_kernel = new (std::nothrow) kernel::LiteKernel(shared_kernel);
      if (lite_kernel == nullptr) {
        MS_LOG(ERROR) << "New LiteKernel for delegate subgraph failed.";
        return RET_NULL_PTR;
      }
      kernel::KernelKey delegate_desc{
        kernel::kDelegate, kernel->inputs()[0]->data_type(), schema::PrimitiveType_NONE, "", "", delegate_};
      lite_kernel->set_desc(delegate_desc);
      dst_kernels->push_back(lite_kernel);
    }
  }
  // Release the cpu kernel that has been replace by delegate subgraph
  for (auto kernel : src_kernels) {
    if (delegate_support[kernel] == true) {
      delete kernel;
    }
  }
  return RET_OK;
}

void Scheduler::FindNodeInoutTensors(const lite::Model::Node &node, std::vector<Tensor *> *inputs,
                                     std::vector<Tensor *> *outputs) {
  MS_ASSERT(inputs != nullptr);
  MS_ASSERT(outputs != nullptr);
  auto in_size = node.input_indices_.size();
  inputs->reserve(in_size);
  for (size_t j = 0; j < in_size; ++j) {
    inputs->emplace_back(src_tensors_->at(node.input_indices_[j]));
  }
  auto out_size = node.output_indices_.size();
  outputs->reserve(out_size);
  for (size_t j = 0; j < out_size; ++j) {
    outputs->emplace_back(src_tensors_->at(node.output_indices_[j]));
  }
}

int Scheduler::InferNodeShape(const lite::Model::Node *node) {
  MS_ASSERT(node != nullptr);
  auto primitive = node->primitive_;
  MS_ASSERT(primitive != nullptr);
  if (IsPartialNode(primitive)) {
    return InferPartialShape(node);
  }
  std::vector<Tensor *> inputs;
  std::vector<Tensor *> outputs;
  FindNodeInoutTensors(*node, &inputs, &outputs);
  auto ret = KernelInferShape(inputs, outputs, node->primitive_, context_->GetProviders());
  if (ret != RET_NOT_SUPPORT) {
    return ret;
  }

  int schema_version = VersionManager::GetInstance()->GetSchemaVersion();
  auto parame_gen =
    PopulateRegistry::GetInstance()->GetParameterCreator(GetPrimitiveType(node->primitive_), schema_version);
  if (parame_gen == nullptr) {
    MS_LOG(ERROR) << "parameter generator is nullptr.";
    return RET_NULL_PTR;
  }
  auto parameter = parame_gen(primitive);
  if (parameter == nullptr) {
    MS_LOG(ERROR) << "PopulateParameter return nullptr, type: " << PrimitiveTypeName(GetPrimitiveType(primitive));
    return RET_ERROR;
  }
  parameter->quant_type_ = node->quant_type_;
  parameter->thread_num_ = context_->thread_num_;

  op_parameters_[node->output_indices_.at(0)] = parameter;
  ret = KernelInferShape(inputs, outputs, parameter);
  if (ret == RET_OK) {
    for (auto &output : outputs) {
      if (output->ElementsNum() >= MAX_MALLOC_SIZE / static_cast<int>(sizeof(int64_t))) {
        MS_LOG(ERROR) << "The size of output tensor is too big";
        return RET_ERROR;
      }
    }
  } else if (ret != RET_INFER_INVALID) {
    free(parameter);
    op_parameters_[node->output_indices_.at(0)] = nullptr;
  }
  return ret;
}

int Scheduler::InferPartialShape(const lite::Model::Node *node) {
  MS_ASSERT(src_model_ != nullptr);
  MS_ASSERT(node != nullptr);
  if (!IsPartialNode(node->primitive_)) {
    MS_LOG(ERROR) << "Node is not a partial";
    return RET_PARAM_INVALID;
  }
  return InferSubGraphShape(GetPartialGraphIndex(node->primitive_));
}

int Scheduler::InferSubGraphShape(size_t subgraph_index) {
  MS_ASSERT(src_model_ != nullptr);
  MS_ASSERT(!src_model_->sub_graphs_.empty());
  MS_ASSERT(src_model_->sub_graphs_.size() > subgraph_index);
  auto subgraph = src_model_->sub_graphs_.at(subgraph_index);
  for (auto node_index : subgraph->node_indices_) {
    auto node = src_model_->all_nodes_[node_index];
    MS_ASSERT(node != nullptr);
    auto *primitive = node->primitive_;
    if (primitive == nullptr) {
      MS_LOG(ERROR) << "Op " << node->name_ << " should exist in model!";
      return RET_ERROR;
    }
    auto type = GetPrimitiveType(primitive);
    auto ret = InferNodeShape(node);
    if (ret == RET_INFER_INVALID) {
      MS_LOG(INFO) << "InferShape interrupted, name: " << node->name_ << ", type: " << PrimitiveTypeName(type)
                   << ", set infer flag to false.";
    } else if (ret != RET_OK) {
      MS_LOG(ERROR) << "InferShape failed, name: " << node->name_ << ", type: " << PrimitiveTypeName(type);
      return RET_INFER_ERR;
    }
  }
  return RET_OK;
}

namespace {
int CastConstTensorData(Tensor *tensor, std::map<Tensor *, Tensor *> *restored_origin_tensors, TypeId dst_data_type) {
#if defined(ENABLE_ARM) && defined(ENABLE_FP16)
  MS_ASSERT(tensor != nullptr);
  MS_ASSERT(tensor->IsConst());
  MS_ASSERT(tensor->data_type() == kNumberTypeFloat32 || tensor->data_type() == kNumberTypeFloat16);
  MS_ASSERT(dst_data_type == kNumberTypeFloat32 || dst_data_type == kNumberTypeFloat16);
  if (tensor->data_type() == dst_data_type) {
    return RET_OK;
  }
  auto origin_data = tensor->data_c();
  MS_ASSERT(origin_data != nullptr);
  auto restore_tensor = Tensor::CopyTensor(*tensor, false);
  restore_tensor->set_data(origin_data);
  restore_tensor->set_own_data(tensor->own_data());
  tensor->set_data(nullptr);
  tensor->set_data_type(dst_data_type);
  auto ret = tensor->MallocData();
  if (RET_OK != ret) {
    MS_LOG(ERROR) << "malloc data failed";
    return ret;
  }
  auto new_tensor_data = tensor->data_c();
  MS_ASSERT(new_tensor_data != nullptr);
  if (dst_data_type == kNumberTypeFloat32) {
    Float16ToFloat32_fp16_handler(origin_data, new_tensor_data, tensor->ElementsNum());
  } else {  // dst_data_type == kNumberTypeFloat16
    Float32ToFloat16_fp16_handler(origin_data, new_tensor_data, tensor->ElementsNum());
  }
  if (restored_origin_tensors->find(tensor) != restored_origin_tensors->end()) {
    MS_LOG(ERROR) << "Tensor " << tensor->tensor_name() << " is already be stored";
    return RET_ERROR;
  }
  (*restored_origin_tensors)[tensor] = restore_tensor;
  return RET_OK;
#else
  return RET_NOT_SUPPORT;
#endif
}

int CastConstTensorsData(const std::vector<Tensor *> &tensors, std::map<Tensor *, Tensor *> *restored_origin_tensors,
                         TypeId dst_data_type) {
  MS_ASSERT(restored_origin_tensors != nullptr);
  if (dst_data_type != kNumberTypeFloat32 && dst_data_type != kNumberTypeFloat16) {
    MS_LOG(ERROR) << "Only support fp32 or fp16 as dst_data_type.";
    return RET_PARAM_INVALID;
  }
  for (auto *tensor : tensors) {
    MS_ASSERT(tensor != nullptr);
    // only cast const tensor
    // tensorlist not support fp16 now
    if (!tensor->IsConst() || tensor->data_type() == kObjectTypeTensorType) {
      continue;
    }
    // only support fp32->fp16 or fp16->fp32
    if (tensor->data_type() != kNumberTypeFloat32 && tensor->data_type() != kNumberTypeFloat16) {
      continue;
    }
    if (tensor->data_type() == kNumberTypeFloat32 && dst_data_type == kNumberTypeFloat16) {
      auto ret = CastConstTensorData(tensor, restored_origin_tensors, kNumberTypeFloat16);
      if (ret != RET_OK) {
        MS_LOG(DEBUG) << "Cast const tensor from fp32 to fp16 failed, tensor name : " << tensor->tensor_name();
        return ret;
      }
    } else if (tensor->data_type() == kNumberTypeFloat16 && dst_data_type == kNumberTypeFloat32) {
      auto ret = CastConstTensorData(tensor, restored_origin_tensors, kNumberTypeFloat32);
      if (ret != RET_OK) {
        MS_LOG(DEBUG) << "Cast const tensor from fp16 to fp32 failed, tensor name : " << tensor->tensor_name();
        return ret;
      }
    } else {
      MS_LOG(DEBUG) << "No need to cast from " << tensor->data_type() << " to " << dst_data_type;
    }
  }
  return RET_OK;
}

int CopyConstTensorData(const std::vector<Tensor *> &tensors, int op_type) {
  // packed kernels such as conv don't need to copy because weight will be packed in kernel
  if (IsPackedOp(op_type)) {
    return RET_OK;
  }
  for (auto *tensor : tensors) {
    // only copy non-copied const tensor
    if (!tensor->IsConst() || tensor->own_data()) {
      continue;
    }
    if (tensor->data_type() == kObjectTypeTensorType) {
      // tensorlist's data is nullptr since ConvertTensors
      // we never set or malloc data of tensorlist but malloc tensors in tensorlist
      MS_ASSERT(tensor->data_c() == nullptr);
    } else {
      auto copy_tensor = Tensor::CopyTensor(*tensor, true);
      if (copy_tensor == nullptr) {
        MS_LOG(ERROR) << "Copy tensor failed";
        return RET_ERROR;
      }
      tensor->FreeData();
      tensor->set_data(copy_tensor->data_c());
      tensor->set_own_data(true);
      copy_tensor->set_data(nullptr);
      delete (copy_tensor);
    }
  }
  return RET_OK;
}

inline void FreeRestoreTensors(std::map<Tensor *, Tensor *> *restored_origin_tensors) {
  MS_ASSERT(restored_origin_tensors != nullptr);
  for (auto &restored_origin_tensor : *restored_origin_tensors) {
    restored_origin_tensor.second->set_data(nullptr);
    delete (restored_origin_tensor.second);
  }
  restored_origin_tensors->clear();
}

inline void RestoreTensorData(std::map<Tensor *, Tensor *> *restored_origin_tensors) {
  MS_ASSERT(restored_origin_tensors != nullptr);
  for (auto &restored_origin_tensor : *restored_origin_tensors) {
    auto *origin_tensor = restored_origin_tensor.first;
    auto *restored_tensor = restored_origin_tensor.second;
    MS_ASSERT(origin_tensor != nullptr);
    MS_ASSERT(restored_tensor != nullptr);
    origin_tensor->FreeData();
    origin_tensor->set_data_type(restored_tensor->data_type());
    origin_tensor->set_data(restored_tensor->data_c());
    origin_tensor->set_own_data(restored_tensor->own_data());
  }
  FreeRestoreTensors(restored_origin_tensors);
}
}  // namespace

int Scheduler::FindCpuKernel(const std::vector<Tensor *> &in_tensors, const std::vector<Tensor *> &out_tensors,
                             OpParameter *op_parameter, const kernel::KernelKey &desc, TypeId kernel_data_type,
                             kernel::LiteKernel **kernel) {
  MS_ASSERT(op_parameter != nullptr);
  auto op_type = op_parameter->type_;
  if (!KernelRegistry::GetInstance()->SupportKernel(desc)) {
    return RET_NOT_SUPPORT;
  }
  kernel::KernelKey cpu_desc = desc;
  if (kernel_data_type == kNumberTypeFloat16) {
    if (!context_->IsCpuFloat16Enabled() ||
        (cpu_desc.data_type != kNumberTypeFloat32 && cpu_desc.data_type != kNumberTypeFloat16)) {
      return RET_NOT_SUPPORT;
    }
    cpu_desc.data_type = kNumberTypeFloat16;
  }
  auto ret = WeightDecoder::DequantNode(op_parameter, in_tensors, kernel_data_type);
  if (ret != RET_OK) {
    MS_LOG(DEBUG) << "Dequant input tensors failed: " << ret;
    return RET_NOT_SUPPORT;
  }
  std::map<Tensor *, Tensor *> restored_origin_tensors;

  ret = CastConstTensorsData(in_tensors, &restored_origin_tensors, kernel_data_type);
  if (ret != RET_OK) {
    MS_LOG(DEBUG) << "CastConstTensorsData failed: " << ret;
    return RET_NOT_SUPPORT;
  }
  if (!is_train_session_) {
    // we don't need to restore tensor for copy data
    ret = CopyConstTensorData(in_tensors, op_type);
    if (ret != RET_OK) {
      MS_LOG(DEBUG) << "CopyConstTensorsData failed: " << ret;
      return RET_NOT_SUPPORT;
    }
  }
  ret = KernelRegistry::GetInstance()->GetKernel(in_tensors, out_tensors, context_, cpu_desc, op_parameter, kernel);
  if (ret == RET_OK) {
    MS_LOG(DEBUG) << "Get TypeId(" << kernel_data_type << ") op success: " << PrimitiveCurVersionTypeName(op_type);
    if (is_train_session_) {
      (*kernel)->Init();
      RestoreTensorData(&restored_origin_tensors);
    } else {
      FreeRestoreTensors(&restored_origin_tensors);
    }
  } else {
    RestoreTensorData(&restored_origin_tensors);
  }
  return ret;
}

int Scheduler::FindGpuKernel(const std::vector<Tensor *> &in_tensors, const std::vector<Tensor *> &out_tensors,
                             OpParameter *op_parameter, const kernel::KernelKey &desc, kernel::LiteKernel **kernel) {
  MS_ASSERT(op_parameter != nullptr);
  MS_ASSERT(kernel != nullptr);
  if (context_->IsGpuEnabled()) {
    // support more data type like int32
    kernel::KernelKey gpu_desc{kernel::KERNEL_ARCH::kGPU, desc.data_type, desc.type};
    if (desc.data_type == kNumberTypeFloat32 && context_->IsGpuFloat16Enabled()) {
      gpu_desc.data_type = kNumberTypeFloat16;
    }

    // weight dequant
    auto ret = WeightDecoder::DequantNode(op_parameter, in_tensors, kNumberTypeFloat32);
    if (ret != RET_OK) {
      MS_LOG(DEBUG) << "Dequant input tensors failed: " << ret;
      return RET_NOT_SUPPORT;
    }
    // we don't need to restore tensor for copy data
    ret = CopyConstTensorData(in_tensors, op_parameter->type_);
    if (ret != RET_OK) {
      MS_LOG(DEBUG) << "CopyConstTensorsData failed: " << ret;
      return RET_NOT_SUPPORT;
    }
    ret = KernelRegistry::GetInstance()->GetKernel(in_tensors, out_tensors, context_, gpu_desc, op_parameter, kernel);
    if (ret == RET_OK) {
      MS_LOG(DEBUG) << "Get gpu op success: " << PrimitiveCurVersionTypeName(gpu_desc.type);
    } else {
      MS_LOG(DEBUG) << "Get gpu op failed, scheduler to cpu: " << PrimitiveCurVersionTypeName(gpu_desc.type);
    }
    return ret;
  }
  return RET_NOT_SUPPORT;
}

int Scheduler::FindNpuKernel(const std::vector<Tensor *> &in_tensors, const std::vector<Tensor *> &out_tensors,
                             OpParameter *op_parameter, const kernel::KernelKey &desc, kernel::LiteKernel **kernel) {
  MS_ASSERT(op_parameter != nullptr);
  kernel::KernelKey npu_desc{kernel::KERNEL_ARCH::kNPU, desc.data_type, desc.type};
  if (context_->IsNpuEnabled()) {
    if (npu_desc.data_type == kNumberTypeFloat16) {
      npu_desc.data_type = kNumberTypeFloat32;
    }
    auto ret = WeightDecoder::DequantNode(op_parameter, in_tensors, kNumberTypeFloat32);
    if (ret != RET_OK) {
      MS_LOG(DEBUG) << "Dequant input tensors failed: " << ret;
      return RET_NOT_SUPPORT;
    }
    for (auto tensor : in_tensors) {
      if (tensor->data_type() == kNumberTypeFloat16) {
        tensor->set_data_type(kNumberTypeFloat32);
      }
    }
    ret = KernelRegistry::GetInstance()->GetKernel(in_tensors, out_tensors, context_, npu_desc, op_parameter, kernel);
    if (ret == RET_OK) {
      MS_LOG(DEBUG) << "Get npu op success: " << PrimitiveCurVersionTypeName(npu_desc.type);
    } else {
      MS_LOG(DEBUG) << "Get npu op failed, scheduler to cpu: " << PrimitiveCurVersionTypeName(npu_desc.type);
    }
    return ret;
  }
  return RET_NOT_SUPPORT;
}

int Scheduler::FindProviderKernel(const std::vector<Tensor *> &in_tensors, const std::vector<Tensor *> &out_tensors,
                                  const Model::Node *node, TypeId data_type, kernel::LiteKernel **kernel) {
  MS_ASSERT(kernel != nullptr);
  int ret = RET_NOT_SUPPORT;
  auto prim_type = GetPrimitiveType(node->primitive_);
  if (prim_type == schema::PrimitiveType_Custom) {
    kernel::KernelKey desc{kernel::KERNEL_ARCH::kCPU, data_type, prim_type, "", ""};
    ret = KernelRegistry::GetInstance()->GetKernel(in_tensors, out_tensors, context_, desc, nullptr, kernel,
                                                   node->primitive_);
    if (ret == RET_OK && *kernel != nullptr) {
      return ret;
    }
    return RET_NOT_SUPPORT;
  }
  if (!context_->IsProviderEnabled()) {
    return ret;
  }
  if (VersionManager::GetInstance()->GetSchemaVersion() == SCHEMA_V0) {
    return ret;
  }
  for (auto &&device : context_->device_list_) {
    if (!device.provider_.empty()) {
      kernel::KernelKey desc{kernel::KERNEL_ARCH::kCPU, data_type, prim_type, device.provider_device_,
                             device.provider_};
      ret = KernelRegistry::GetInstance()->GetKernel(in_tensors, out_tensors, context_, desc, nullptr, kernel,
                                                     node->primitive_);
      if (ret == RET_OK && *kernel != nullptr) {
        return ret;
      }
    }
  }

  return RET_NOT_SUPPORT;
}

kernel::LiteKernel *Scheduler::FindBackendKernel(const std::vector<Tensor *> &in_tensors,
                                                 const std::vector<Tensor *> &out_tensors, const Model::Node *node,
                                                 TypeId prefer_data_type) {
  MS_ASSERT(node != nullptr);
  // why we need this
  TypeId data_type =
    (node->quant_type_ == schema::QuantType_QUANT_WEIGHT) ? kNumberTypeFloat32 : GetFirstFp32Fp16OrInt8Type(in_tensors);
  kernel::LiteKernel *kernel = nullptr;
  int status;
  status = FindProviderKernel(in_tensors, out_tensors, node, data_type, &kernel);
  if (status == RET_OK && kernel != nullptr) {
    return kernel;
  }
  MS_ASSERT(!node->output_indices_.empty());
  OpParameter *op_parameter = op_parameters_[node->output_indices_.at(0)];
  if (op_parameter == nullptr) {
    MS_LOG(ERROR) << "Can not find OpParameter!type: " << PrimitiveTypeName(GetPrimitiveType(node->primitive_));
    return nullptr;
  }
  int kernel_thread_count = op_parameter->thread_num_;
  op_parameter->is_train_session_ = is_train_session_;
  kernel::KernelKey desc{kernel::KERNEL_ARCH::kCPU, data_type, static_cast<schema::PrimitiveType>(op_parameter->type_)};
#ifdef SUPPORT_GPU
  if (node->device_type_ == DT_GPU || node->device_type_ == kDefaultDeviceType) {
    status = FindGpuKernel(in_tensors, out_tensors, op_parameter, desc, &kernel);
    if (status == RET_OK) {
      return kernel;
    } else {
      MS_LOG(DEBUG) << "Get gpu op failed, scheduler to cpu: " << PrimitiveCurVersionTypeName(desc.type) << " "
                    << node->name_;
      if (status == RET_ERROR) {
        auto ret = InferNodeShape(node);
        if (ret == RET_INFER_INVALID || ret == RET_OK) {
          op_parameter = op_parameters_[node->output_indices_.at(0)];
          op_parameter->thread_num_ = kernel_thread_count;
        } else {
          MS_LOG(ERROR) << "Try repeat infer fail: " << node->name_;
          return nullptr;
        }
      }
    }
  }
#endif
#ifdef SUPPORT_NPU
  if (node->device_type_ == DT_NPU || node->device_type_ == kDefaultDeviceType) {
    status = FindNpuKernel(in_tensors, out_tensors, op_parameter, desc, &kernel);
    if (status == RET_OK) {
      return kernel;
    } else {
      MS_LOG(DEBUG) << "Get npu op failed, scheduler to cpu: " << PrimitiveCurVersionTypeName(desc.type) << " "
                    << node->name_;
      if (status == RET_ERROR) {
        auto ret = InferNodeShape(node);
        if (ret == RET_INFER_INVALID || ret == RET_OK) {
          op_parameter = op_parameters_[node->output_indices_.at(0)];
          op_parameter->thread_num_ = kernel_thread_count;
        } else {
          MS_LOG(ERROR) << "Try repeat infer fail: " << node->name_;
          return nullptr;
        }
      }
    }
  }
#endif
  if ((prefer_data_type == kNumberTypeFloat16 || prefer_data_type == kTypeUnknown) &&
      ((is_train_session_ == false) || (sched_cb_ && sched_cb_->SchedFp16Kernel(node)))) {
    status = FindCpuKernel(in_tensors, out_tensors, op_parameter, desc, kNumberTypeFloat16, &kernel);
    if (status == RET_OK) {
      return kernel;
    } else {
      MS_LOG(DEBUG) << "Get fp16 op failed, scheduler to cpu: " << PrimitiveCurVersionTypeName(desc.type) << " "
                    << node->name_;
      if (status == RET_ERROR) {
        auto ret = InferNodeShape(node);
        if (ret == RET_INFER_INVALID || ret == RET_OK) {
          op_parameter = op_parameters_[node->output_indices_.at(0)];
          op_parameter->thread_num_ = kernel_thread_count;
        } else {
          MS_LOG(ERROR) << "Try repeat infer fail: " << node->name_;
          return nullptr;
        }
      }
    }
  }
  if (data_type == kNumberTypeFloat16) {
    MS_LOG(DEBUG) << "Get fp16 op failed, back to fp32 op.";
    desc.data_type = kNumberTypeFloat32;
  }
  if (prefer_data_type == kNumberTypeFloat32 || prefer_data_type == kTypeUnknown) {
    status = FindCpuKernel(in_tensors, out_tensors, op_parameter, desc, kNumberTypeFloat32, &kernel);
    if (status == RET_OK) {
      return kernel;
    } else if (status == RET_ERROR) {
      auto ret = InferNodeShape(node);
      if (!(ret == RET_INFER_INVALID || ret == RET_OK)) {
        MS_LOG(ERROR) << "Try repeat infer fail: " << node->name_;
      }
    }
  }
  return nullptr;
}

kernel::LiteKernel *Scheduler::SchedulePartialToKernel(const lite::Model::Node *src_node) {
  MS_ASSERT(src_model_ != nullptr);
  MS_ASSERT(src_node != nullptr);
  auto *primitive = src_node->primitive_;
  MS_ASSERT(primitive != nullptr);
  if (!IsPartialNode(primitive)) {
    return nullptr;
  }
  auto sub_graph_index = GetPartialGraphIndex(src_node->primitive_);
  std::vector<kernel::LiteKernel *> sub_kernels;
  std::vector<lite::Tensor *> in_tensors;
  std::vector<lite::Tensor *> out_tensors;
  auto ret = ScheduleSubGraphToKernels(sub_graph_index, &sub_kernels, &in_tensors, &out_tensors, kNumberTypeFloat32);
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "Schedule partial failed, name: " << src_node->name_;
    return nullptr;
  }

  FindAllInoutKernels(sub_kernels);
  ret = RunPass(&sub_kernels);
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "SchedulePartialToKernel run pass failed.";
    return nullptr;
  }

  auto cur_sub_graph_type = mindspore::lite::Scheduler::GetKernelSubGraphType(sub_kernels.front());
  auto subgraph = CreateSubGraphKernel(sub_kernels, &in_tensors, &out_tensors, cur_sub_graph_type);
  subgraph->set_name("subgraph_" + src_node->name_);
  return subgraph;
}

kernel::LiteKernel *Scheduler::ScheduleNodeToKernel(const lite::Model::Node *src_node, TypeId prefer_data_type) {
  std::vector<Tensor *> inputs;
  std::vector<Tensor *> outputs;
  FindNodeInoutTensors(*src_node, &inputs, &outputs);
  auto *kernel = this->FindBackendKernel(inputs, outputs, src_node, prefer_data_type);
  if (kernel == nullptr) {
    MS_LOG(ERROR) << "FindBackendKernel return nullptr, name: " << src_node->name_
                  << ", type: " << PrimitiveTypeName(GetPrimitiveType(src_node->primitive_));
    return nullptr;
  }
  SetKernelTensorDataType(kernel);
  kernel->set_name(src_node->name_);
  return kernel;
}

int Scheduler::ScheduleSubGraphToKernels(size_t subgraph_index, std::vector<kernel::LiteKernel *> *dst_kernels,
                                         std::vector<lite::Tensor *> *in_tensors,
                                         std::vector<lite::Tensor *> *out_tensors, TypeId prefer_data_type) {
  MS_ASSERT(src_model_ != nullptr);
  MS_ASSERT(!src_model_->sub_graphs_.empty());
  MS_ASSERT(src_model_->sub_graphs_.size() > subgraph_index);
  MS_ASSERT(dst_kernels != nullptr);
  MS_ASSERT(dst_kernels->empty());
  auto subgraph = src_model_->sub_graphs_.at(subgraph_index);
  for (auto node_index : subgraph->node_indices_) {
    auto ret = RET_OK;
    auto node = src_model_->all_nodes_[node_index];
    MS_ASSERT(node != nullptr);
    auto *primitive = node->primitive_;
    MS_ASSERT(primitive != nullptr);
    kernel::LiteKernel *kernel = nullptr;
    auto prim_type = GetPrimitiveType(primitive);
    if (IsPartialNode(primitive)) {  // sub_graph
      kernel = SchedulePartialToKernel(node);
    } else {  // kernel
      kernel = ScheduleNodeToKernel(node, prefer_data_type);
      if ((kernel != nullptr) && (!is_train_session_)) {
        ret = kernel->Init();
      }
    }
    if (kernel == nullptr || ret != RET_OK) {
      MS_LOG(ERROR) << "FindBackendKernel return nullptr, name: " << node->name_
                    << ", type: " << PrimitiveTypeName(prim_type);
      return RET_ERROR;
    }
    kernel->set_is_model_output(IsContain(graph_output_node_indexes_, size_t(node_index)));
    dst_kernels->emplace_back(kernel);
    primitives_.emplace(kernel->kernel(), static_cast<const schema::Primitive *>(primitive));
  }
  if (in_tensors != nullptr) {
    std::transform(subgraph->input_indices_.begin(), subgraph->input_indices_.end(), std::back_inserter(*in_tensors),
                   [&](const uint32_t index) { return this->src_tensors_->at(index); });
  }
  if (out_tensors != nullptr) {
    std::transform(subgraph->output_indices_.begin(), subgraph->output_indices_.end(), std::back_inserter(*out_tensors),
                   [&](const uint32_t index) { return this->src_tensors_->at(index); });
  }
  return RET_OK;
}

bool Scheduler::KernelFitCurrentSubGraph(const kernel::SubGraphType subgraph_type, const kernel::LiteKernel &kernel) {
  switch (subgraph_type) {
    case kernel::SubGraphType::kNotSubGraph:
    case kernel::SubGraphType::kApuSubGraph:
      return false;
    case kernel::SubGraphType::kGpuSubGraph:
      return kernel.desc().arch == kernel::KERNEL_ARCH::kGPU;
    case kernel::SubGraphType::kNpuSubGraph:
      return kernel.desc().arch == kernel::KERNEL_ARCH::kNPU;
    case kernel::SubGraphType::kCpuFP16SubGraph: {
      auto desc = kernel.desc();
      if (desc.arch != kernel::KERNEL_ARCH::kCPU) {
        return false;
      }
      return (desc.data_type == kNumberTypeFloat16 || desc.data_type == kNumberTypeInt32 ||
              desc.data_type == kNumberTypeInt || desc.data_type == kNumberTypeBool);
    }
    case kernel::SubGraphType::kCpuFP32SubGraph: {
      auto desc = kernel.desc();
      if (desc.arch != kernel::KERNEL_ARCH::kCPU) {
        return false;
      }
      return (desc.data_type == kNumberTypeFloat32 || desc.data_type == kNumberTypeFloat ||
              desc.data_type == kNumberTypeInt8 || desc.data_type == kNumberTypeInt ||
              desc.data_type == kNumberTypeInt32 || desc.data_type == kNumberTypeInt64 ||
              desc.data_type == kNumberTypeUInt8 || desc.data_type == kNumberTypeBool);
    }
    default:
      return false;
  }
}

std::vector<kernel::LiteKernel *> Scheduler::FindAllSubGraphKernels(
  std::vector<kernel::LiteKernel *> head_kernels, std::map<const kernel::LiteKernel *, bool> *sinked_kernel_map) {
  std::vector<kernel::LiteKernel *> sub_kernels;

  for (kernel::LiteKernel *head_kernel : head_kernels) {
    MS_ASSERT(head_kernel != nullptr);
    MS_ASSERT(sinked_kernel_map != nullptr);
    if (head_kernel->type() == schema::PrimitiveType_Switch || head_kernel->type() == schema::PrimitiveType_Merge) {
      (*sinked_kernel_map)[head_kernel] = true;
      sub_kernels.emplace_back(head_kernel);
      return sub_kernels;
    }
    std::queue<kernel::LiteKernel *> kernel_queue;
    kernel_queue.emplace(head_kernel);
    auto cur_sub_graph_type = mindspore::lite::Scheduler::GetKernelSubGraphType(head_kernel);
    while (!kernel_queue.empty()) {
      auto cur_kernel = kernel_queue.front();
      kernel_queue.pop();
      (*sinked_kernel_map)[cur_kernel] = true;
      sub_kernels.emplace_back(cur_kernel);
      auto post_kernels = cur_kernel->out_kernels();
      for (auto post_kernel : post_kernels) {
        if (post_kernel->subgraph_type() != kernel::kNotSubGraph ||
            post_kernel->type() == schema::PrimitiveType_Merge || post_kernel->type() == schema::PrimitiveType_Switch) {
          continue;
        }
        if (cur_sub_graph_type == mindspore::lite::Scheduler::GetKernelSubGraphType(post_kernel)) {
          auto post_kernel_inputs = post_kernel->in_kernels();
          if (std::all_of(post_kernel_inputs.begin(), post_kernel_inputs.end(),
                          [&](kernel::LiteKernel *kernel) { return (*sinked_kernel_map)[kernel]; })) {
            kernel_queue.emplace(post_kernel);
          }
        }
      }
    }
  }
  return sub_kernels;
}

int Scheduler::ConstructSubGraphs(std::vector<kernel::LiteKernel *> src_kernel,
                                  std::vector<kernel::LiteKernel *> *dst_kernel,
                                  std::map<const kernel::LiteKernel *, bool> *is_kernel_finish) {
  for (auto kernel : src_kernel) {
    (*is_kernel_finish)[kernel] = false;
  }
  while (true) {
    std::vector<kernel::LiteKernel *> head_kernels; /* support one-head-kernel in subgraph */
    auto head_kernel_iter = std::find_if(src_kernel.begin(), src_kernel.end(), [&](const kernel::LiteKernel *kernel) {
      auto kernel_inputs = kernel->in_kernels();
      if ((*is_kernel_finish)[kernel]) {
        return false;
      }
      if (std::find(head_kernels.begin(), head_kernels.end(), kernel) != head_kernels.end()) {
        return false;
      }
      // when merge is removed, this if is removed automatically
      if (kernel->type() == schema::PrimitiveType_Merge) {
        return MergeOpIsReady(kernel, (*is_kernel_finish));
      } else {
        return std::all_of(kernel_inputs.begin(), kernel_inputs.end(),
                           [&](kernel::LiteKernel *kernel) { return (*is_kernel_finish)[kernel]; });
      }
    });
    if (head_kernel_iter == src_kernel.end()) {
      break;
    }

    auto head_kernel = *head_kernel_iter;
    if (head_kernel->subgraph_type() != kernel::kNotSubGraph) {
      (*is_kernel_finish)[head_kernel] = true;
      dst_kernel->push_back(head_kernel);
      continue;
    }
    if (head_kernel->desc().arch == mindspore::kernel::kAPU) {
      MS_LOG(ERROR) << "Not support APU now";
      return RET_NOT_SUPPORT;
    }

    head_kernels.push_back(head_kernel);

    auto subgraph_delegate = head_kernel->desc().delegate;
    if (subgraph_delegate != nullptr) {
      dst_kernel->emplace_back(head_kernel);
      (*is_kernel_finish)[head_kernel] = true;
    } else {
      auto cur_sub_graph_type = mindspore::lite::Scheduler::GetKernelSubGraphType(head_kernels[0]);
      auto sub_kernels = FindAllSubGraphKernels(head_kernels, is_kernel_finish);
      auto subgraph = CreateSubGraphKernel(sub_kernels, nullptr, nullptr, cur_sub_graph_type);
      if (subgraph == nullptr) {
        MS_LOG(ERROR) << "Create SubGraphKernel failed";
        return RET_ERROR;
      }
      dst_kernel->emplace_back(subgraph);
    }
  } /* end when all kernel converted */

  for (auto *subgraph : *dst_kernel) {
    auto subgraph_delegate = subgraph->desc().delegate;
    if (subgraph_delegate == nullptr) {
      auto ret = subgraph->Init();
      if (ret != RET_OK) {
        MS_LOG(ERROR) << "Init SubGraph failed: " << ret;
        return ret;
      }
    }
  }
  return RET_OK;
}

bool Scheduler::MergeOpIsReady(const kernel::LiteKernel *kernel,
                               std::map<const kernel::LiteKernel *, bool> is_kernel_finish) {
  MS_ASSERT(kernel != nullptr);
  std::map<const lite::Tensor *, bool> merge_in_tensors_map;
  for (auto merge_in_tensor : kernel->in_tensors()) {
    merge_in_tensors_map[merge_in_tensor] = false;
    if (merge_in_tensor->category() == Tensor::CONST_TENSOR || merge_in_tensor->category() == Tensor::CONST_SCALAR ||
        merge_in_tensor->category() == Tensor::GRAPH_INPUT) {
      merge_in_tensors_map[merge_in_tensor] = true;
    }
    for (auto merge_in_kernel : kernel->in_kernels()) {
      for (auto tensor : merge_in_kernel->out_tensors()) {
        if (tensor == merge_in_tensor && is_kernel_finish[merge_in_kernel]) {
          merge_in_tensors_map[merge_in_tensor] = true;
        }
      }
    }
  }
  auto kernel_in_tensors_num = kernel->in_tensors().size();
  auto &in_tensors = kernel->in_tensors();
  return std::all_of(in_tensors.begin(), in_tensors.begin() + kernel_in_tensors_num / 2,
                     [&](lite::Tensor *in_tensor) { return merge_in_tensors_map[in_tensor]; }) ||
         std::all_of(in_tensors.begin() + kernel_in_tensors_num / 2, in_tensors.end(),
                     [&](lite::Tensor *in_tensor) { return merge_in_tensors_map[in_tensor]; });
}

kernel::SubGraphKernel *Scheduler::CreateSubGraphKernel(const std::vector<kernel::LiteKernel *> &kernels,
                                                        const std::vector<lite::Tensor *> *in_tensors,
                                                        const std::vector<lite::Tensor *> *out_tensors,
                                                        kernel::SubGraphType type) {
  if (type == kernel::kApuSubGraph) {
    return nullptr;
  }
  std::vector<Tensor *> input_tensors;
  std::vector<Tensor *> output_tensors;
  if (in_tensors != nullptr) {
    input_tensors = *in_tensors;
  } else {
    input_tensors = kernel::LiteKernelUtil::SubgraphInputTensors(kernels);
  }
  if (out_tensors != nullptr) {
    output_tensors = *out_tensors;
  } else {
    output_tensors = kernel::LiteKernelUtil::SubgraphOutputTensors(kernels);
  }
  auto innerkernel = new (std::nothrow) kernel::InnerKernel(nullptr, input_tensors, output_tensors, context_);
  if (innerkernel == nullptr) {
    return nullptr;
  }
  std::vector<kernel::LiteKernel *> input_kernels = kernel::LiteKernelUtil::SubgraphInputNodes(kernels);
  std::vector<kernel::LiteKernel *> output_kernels = kernel::LiteKernelUtil::SubgraphOutputNodes(kernels);
  if (type == kernel::kCustomSubGraph) {
    return CreateCustomSubGraph(std::move(input_kernels), std::move(output_kernels), kernels, innerkernel);
  }
  if (type == kernel::kGpuSubGraph) {
#if GPU_OPENCL
    auto sub_kernel = new (std::nothrow) kernel::OpenCLSubGraph(input_kernels, output_kernels, kernels, innerkernel);
    if (sub_kernel == nullptr) {
      MS_LOG(ERROR) << "Create OpenCLSubGraph failed";
      delete innerkernel;
      return nullptr;
    }
    return sub_kernel;
#elif GPU_VULKAN
    delete innerkernel;
    return nullptr;
#else
    delete innerkernel;
    return nullptr;
#endif
  }
  if (type == kernel::kNpuSubGraph) {
#if SUPPORT_NPU
    auto sub_kernel =
      new (std::nothrow) kernel::SubGraphNpuKernel(input_kernels, output_kernels, kernels, innerkernel, npu_manager_);
    if (sub_kernel == nullptr) {
      MS_LOG(ERROR) << "NPU subgraph new failed.";
      delete innerkernel;
      return nullptr;
    }
    return sub_kernel;
#else
    delete innerkernel;
    return nullptr;
#endif
  }
  if (type == kernel::kCpuFP16SubGraph) {
#ifdef ENABLE_FP16
    auto sub_kernel = new (std::nothrow) kernel::CpuFp16SubGraph(input_kernels, output_kernels, kernels, innerkernel);
    if (sub_kernel == nullptr) {
      MS_LOG(ERROR) << "FP16 subgraph new failed.";
      delete innerkernel;
      return nullptr;
    }
    for (auto out_tensor : output_tensors) {
      if (out_tensor->data_type() == kNumberTypeFloat32) {
        out_tensor->set_data_type(kNumberTypeFloat16);
      }
    }
    return sub_kernel;
#else
    delete innerkernel;
    MS_LOG(ERROR) << "FP16 subgraph is not supported!";
    return nullptr;
#endif
  }
  if (type == kernel::kCpuFP32SubGraph) {
    auto sub_kernel = new (std::nothrow) kernel::CpuFp32SubGraph(input_kernels, output_kernels, kernels, innerkernel);
    if (sub_kernel == nullptr) {
      MS_LOG(ERROR) << "FP32 subgraph new failed.";
      delete innerkernel;
      return nullptr;
    }
    return sub_kernel;
  }
  return nullptr;
}

TypeId Scheduler::GetFirstFp32Fp16OrInt8Type(const std::vector<Tensor *> &in_tensors) {
  for (const auto &tensor : in_tensors) {
    auto dtype = tensor->data_type();
    if (dtype == kObjectTypeString) {
      return kNumberTypeFloat32;
    }
    if (dtype == kObjectTypeTensorType) {
      auto tensor_list = reinterpret_cast<TensorList *>(tensor);
      auto tensor_list_dtype = tensor_list->data_type();
      if (tensor_list_dtype == kNumberTypeFloat32 || tensor_list_dtype == kNumberTypeFloat16 ||
          tensor_list_dtype == kNumberTypeInt8 || tensor_list_dtype == kNumberTypeInt32 ||
          tensor_list_dtype == kNumberTypeBool) {
        return tensor_list_dtype;
      }
    }
    if (dtype == kNumberTypeFloat32 || dtype == kNumberTypeFloat16 || dtype == kNumberTypeInt8 ||
        dtype == kNumberTypeInt32 || dtype == kNumberTypeBool) {
      return dtype;
    }
  }
  MS_ASSERT(!in_tensors.empty());
  return in_tensors[0]->data_type();
}

void Scheduler::SetKernelTensorDataType(kernel::LiteKernel *kernel) {
  MS_ASSERT(kernel != nullptr);
  if (kernel->desc().arch != kernel::KERNEL_ARCH::kCPU) {
    return;
  }
  if (kernel->desc().data_type == kNumberTypeFloat16) {
    for (auto tensor : kernel->out_tensors()) {
      if (tensor->data_type() == kNumberTypeFloat32) {
        tensor->set_data_type(kNumberTypeFloat16);
      }
    }
  } else if (kernel->desc().data_type == kNumberTypeFloat32) {
    for (auto tensor : kernel->in_tensors()) {
      if (!tensor->IsConst() && tensor->data_type() == kNumberTypeFloat16) {
        tensor->set_data_type(kNumberTypeFloat32);
      }
    }
    for (auto tensor : kernel->out_tensors()) {
      if (tensor->data_type() == kNumberTypeFloat16) {
        tensor->set_data_type(kNumberTypeFloat32);
      }
    }
  }
}

kernel::SubGraphType Scheduler::GetKernelSubGraphType(const kernel::LiteKernel *kernel) {
  if (kernel == nullptr) {
    return kernel::kNotSubGraph;
  }

  auto desc = kernel->desc();
  if (desc.provider != kernel::kBuiltin) {
    return kernel::kCustomSubGraph;
  }
  if (desc.arch == kernel::KERNEL_ARCH::kGPU) {
    return kernel::kGpuSubGraph;
  } else if (desc.arch == kernel::KERNEL_ARCH::kNPU) {
    return kernel::kNpuSubGraph;
  } else if (desc.arch == kernel::KERNEL_ARCH::kAPU) {
    return kernel::kApuSubGraph;
  } else if (desc.arch == kernel::KERNEL_ARCH::kCPU) {
    if (desc.data_type == kNumberTypeFloat16) {
      return kernel::kCpuFP16SubGraph;
    } else if (desc.data_type == kNumberTypeFloat32 || desc.data_type == kNumberTypeInt8 ||
               desc.data_type == kNumberTypeInt32 || desc.data_type == kNumberTypeInt64 ||
               desc.data_type == kNumberTypeUInt8 || desc.data_type == kNumberTypeBool) {
      return kernel::kCpuFP32SubGraph;
    }
  }
  return kernel::kNotSubGraph;
}

void Scheduler::FindAllInoutKernels(const std::vector<kernel::LiteKernel *> &kernels) {
  for (auto *kernel : kernels) {
    MS_ASSERT(kernel != nullptr);
    kernel->FindInoutKernels(kernels);
  }
}

int Scheduler::RunPass(std::vector<kernel::LiteKernel *> *dst_kernels) {
  MS_ASSERT(dst_kernels != nullptr);
  int ret = RET_OK;
#if SUPPORT_NPU
  if (!context_->IsNpuEnabled()) {
    return RET_OK;
  }
  auto transform_pass = new (std::nothrow) NPUTransformPass(context_, dst_kernels, src_tensors_);
  if (transform_pass == nullptr) {
    MS_LOG(ERROR) << "transform_pass is nullptr";
    return RET_ERROR;
  }
  MS_ASSERT(npu_pass_manager_ != nullptr);
  npu_pass_manager_->AddPass(transform_pass);
  auto concat_format_pass = new (std::nothrow) NPUInsertTransformPass(context_, dst_kernels, src_tensors_);
  if (concat_format_pass == nullptr) {
    MS_LOG(ERROR) << "concat_format_pass is nullptr";
    return RET_ERROR;
  }
  npu_pass_manager_->AddPass(concat_format_pass);
  auto fusion_pass = new (std::nothrow) NPUFusionPass(dst_kernels);
  if (fusion_pass == nullptr) {
    MS_LOG(ERROR) << "fusion_pass is nullptr";
    return RET_ERROR;
  }
  npu_pass_manager_->AddPass(fusion_pass);

  ret = npu_pass_manager_->Run();
  npu_pass_manager_->Clear();
#endif
  return ret;
}
}  // namespace mindspore::lite
