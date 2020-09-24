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
#include <vector>
#include <string>
#include <algorithm>
#include "include/errorcode.h"
#include "src/kernel_registry.h"
#include "src/common/graph_util.h"
#include "src/common/utils.h"
#if SUPPORT_GPU
#include "src/runtime/kernel/opencl/subgraph_opencl_kernel.h"
#include "src/runtime/opencl/opencl_runtime.h"
#endif

namespace mindspore::lite {
int Scheduler::Schedule(const lite::Model *model, std::vector<Tensor *> *tensors,
                        std::vector<kernel::LiteKernel *> *kernels) {
  // 1. op ---> kernel
  // 2. sub graph
  // 3. kernels (kernels --> subGraph)
  int ret = InferShape(model, tensors);
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "op infer shape failed.";
    return RET_ERROR;
  }
  ret = InitOp2Kernel(model, tensors, kernels);
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "init op to kernel failed.";
    return RET_ERROR;
  }

  kernel::LiteKernelUtil::TopologicalSortKernels(*kernels);

  ConstructSubgraphs(kernels);

  MS_LOG(DEBUG) << "schedule kernels success.";
  return RET_OK;
}

int Scheduler::ReSizeKernels(const std::vector<kernel::LiteKernel *> &kernels) {
  bool infer_shape_interrupt = false;
  for (size_t i = 0; i < kernels.size(); ++i) {
    if (kernels[i] == nullptr) {
      MS_LOG(ERROR) << "input kernel is nullptr!";
      return RET_ERROR;
    }
    auto primitive = const_cast<mindspore::lite::PrimitiveC *>(kernels[i]->GetPrimitive());
    if (primitive == nullptr) {
      MS_LOG(ERROR) << "kernel(" << kernels[i]->name() << ")'s primitive is nullptr!";
      return RET_ERROR;
    }
    std::vector<Tensor *> &inputs = kernels[i]->in_tensors();
    std::vector<Tensor *> &outputs = kernels[i]->out_tensors();
    for (size_t j = 0; j < outputs.size(); j++) {
      outputs[j]->FreeData();
    }
    primitive->SetInferFlag(!infer_shape_interrupt);
    auto ret = primitive->InferShape(inputs, outputs);
    if (ret == RET_INFER_INVALID) {
      MS_LOG(INFO) << "InferShape shouldn't be done before runtime, type:"
                   << schema::EnumNamePrimitiveType(static_cast<schema::PrimitiveType>(primitive->Type()))
                   << "flag set to false.";
      primitive->SetInferFlag(false);
      infer_shape_interrupt = true;
    } else if (ret != RET_OK) {
      MS_LOG(ERROR) << "InferShape failed, type: "
                    << schema::EnumNamePrimitiveType(static_cast<schema::PrimitiveType>(primitive->Type()));
      return RET_INFER_ERR;
    }
    if (!infer_shape_interrupt) {
      ret = kernels[i]->ReSize();
      if (ret != RET_OK) {
        MS_LOG(ERROR) << "kernel " << kernels[i]->name() << " resize fail!ret = " << ret;
        return ret;
      }
    }
  }
  return RET_OK;
}

int Scheduler::InferShape(const lite::Model *model, std::vector<Tensor *> *tensors) {
  MS_ASSERT(model != nullptr);
  MS_ASSERT(tensors != nullptr);
  bool infer_shape_interrupt = false;
  uint32_t kernelCount = model->nodes_.size();
  for (uint32_t i = 0; i < kernelCount; ++i) {
    auto node = model->nodes_[i];
    MS_ASSERT(node != nullptr);
    std::vector<Tensor *> inputs;
    std::vector<Tensor *> outputs;
    auto in_size = node->input_indices_.size();
    for (size_t j = 0; j < in_size; ++j) {
      inputs.emplace_back(tensors->at(node->input_indices_[j]));
    }
    auto out_size = node->output_indices_.size();
    for (size_t j = 0; j < out_size; ++j) {
      outputs.emplace_back(tensors->at(node->output_indices_[j]));
    }
    auto *primitive = node->primitive_;
    if (primitive == nullptr) {
      MS_LOG(ERROR) << "Op " << node->name_ << " should exist in model!";
      return RET_ERROR;
    }
    primitive->SetInferFlag(!infer_shape_interrupt);
    auto ret = primitive->InferShape(inputs, outputs);
    if (ret == RET_INFER_INVALID) {
      MS_LOG(INFO) << "InferShape shouldn't be done before runtime, name: " << node->name_
                   << ", type: " << schema::EnumNamePrimitiveType(static_cast<schema::PrimitiveType>(primitive->Type()))
                   << "flag set to false.";
      primitive->SetInferFlag(false);
      infer_shape_interrupt = true;
    } else if (ret != RET_OK) {
      MS_LOG(ERROR) << "InferShape failed, name: " << node->name_ << ", type: "
                    << schema::EnumNamePrimitiveType(static_cast<schema::PrimitiveType>(primitive->Type()));
      return RET_INFER_ERR;
    }
  }

  return RET_OK;
}

int Scheduler::InitOp2Kernel(const lite::Model *model, std::vector<Tensor *> *tensors,
                             std::vector<kernel::LiteKernel *> *kernels) {
  MS_ASSERT(model != nullptr);
  MS_ASSERT(tensors != nullptr);
  uint32_t kernelCount = model->nodes_.size();
  auto graph_output_node_indexes = GetGraphOutputNodes(model);
  for (uint32_t i = 0; i < kernelCount; ++i) {
    auto node = model->nodes_[i];
    MS_ASSERT(node != nullptr);
    std::vector<Tensor *> inputs;
    std::vector<Tensor *> outputs;
    auto in_size = node->input_indices_.size();
    for (size_t j = 0; j < in_size; ++j) {
      inputs.emplace_back(tensors->at(node->input_indices_[j]));
    }
    auto out_size = node->output_indices_.size();
    for (size_t j = 0; j < out_size; ++j) {
      outputs.emplace_back(tensors->at(node->output_indices_[j]));
    }
    auto *primitive = node->primitive_;
    MS_ASSERT(primitive != nullptr);
    auto *kernel = this->ScheduleNode(inputs, outputs, primitive, node);
    if (kernel == nullptr) {
      MS_LOG(ERROR) << "ScheduleNode return nullptr, name: " << node->name_ << ", type: "
                    << schema::EnumNamePrimitiveType(static_cast<schema::PrimitiveType>(primitive->Type()));
      return RET_ERROR;
    }
    SetKernelTensorDataType(kernel);
    kernel->set_name(node->name_);
    kernel->set_is_model_output(IsContain(graph_output_node_indexes, size_t(i)));
    kernels->emplace_back(kernel);
  }

  return RET_OK;
}

void Scheduler::ConstructSubgraphs(std::vector<kernel::LiteKernel *> *kernels) {
  uint32_t kernel_count = kernels->size();
  std::vector<kernel::LiteKernel *> sub_kernels;
  std::vector<std::vector<kernel::LiteKernel *>> sub_kernels_list;

  kernel::KERNEL_ARCH prev_arch = kernels->front()->desc().arch;
  for (uint32_t i = 0; i < kernel_count; ++i) {
    auto curr_kernel = kernels->at(i);
    auto curr_arch = curr_kernel->desc().arch;
    if (curr_arch == prev_arch) {
      sub_kernels.emplace_back(curr_kernel);
    }
    if ((curr_arch != prev_arch) || (i == kernel_count - 1)) {
      sub_kernels_list.emplace_back(sub_kernels);
      sub_kernels.clear();
      sub_kernels.emplace_back(curr_kernel);
    }
    prev_arch = curr_arch;
  }

  std::vector<kernel::LiteKernel *> subgraph_kernels;
  size_t sub_cnt{0};
  for (auto temp_kernels : sub_kernels_list) {
    std::vector<Tensor *> output_tensor = kernel::LiteKernelUtil::SubgraphOutputTensors(temp_kernels);
    for (auto tensor : output_tensor) {
      if (context_->float16_priority && tensor->data_type() == kNumberTypeFloat16) {
        tensor->set_data_type(kNumberTypeFloat32);
      }
    }
    kernel::KERNEL_ARCH arch = temp_kernels.front()->desc().arch;
    if (arch == kernel::KERNEL_ARCH::kCPU) {
      for (auto kernel : temp_kernels) {
        for (auto tensor : kernel->out_tensors()) {
          tensor->set_allocator(context_->allocator.get());
        }
      }
      std::copy(temp_kernels.begin(), temp_kernels.end(), std::back_inserter(subgraph_kernels));
    } else {
      auto subgraph_kernel = CreateSubKernel(temp_kernels, arch);
      subgraph_kernels.emplace_back(subgraph_kernel);
      std::string arch_name = (arch == kernel::KERNEL_ARCH::kGPU) ? "GPU" : "NPU";
      MS_LOG(INFO) << arch_name << " subgraph id" << sub_cnt << " created.";
    }
    ++sub_cnt;
  }
  kernels->clear();
  kernels->insert(kernels->begin(), subgraph_kernels.begin(), subgraph_kernels.end());
}

kernel::LiteKernel *Scheduler::CreateSubKernel(const std::vector<kernel::LiteKernel *> &kernels,
                                               kernel::KERNEL_ARCH arch) {
  kernel::LiteKernel *sub_kernel = nullptr;
#if SUPPORT_GPU
  if (arch == kernel::KERNEL_ARCH::kGPU) {
    std::vector<Tensor *> input_tensors = kernel::LiteKernelUtil::SubgraphInputTensors(kernels);
    std::vector<Tensor *> output_tensors = kernel::LiteKernelUtil::SubgraphOutputTensors(kernels);
    std::vector<kernel::LiteKernel *> input_kernels = kernel::LiteKernelUtil::SubgraphInputKernels(kernels);
    std::vector<kernel::LiteKernel *> output_kernels = kernel::LiteKernelUtil::SubgraphOutputKernels(kernels);
    sub_kernel = new kernel::SubGraphOpenCLKernel(input_tensors, output_tensors, input_kernels, output_kernels, kernels,
                                                  context_, nullptr);
    sub_kernel->Init();
  } else if (arch == kernel::KERNEL_ARCH::kNPU) {
    MS_LOG(ERROR) << "NPU kernel is not supported";
  } else {
    MS_LOG(ERROR) << "unsupported kernel arch: " << arch;
  }
#endif
  return sub_kernel;
}

kernel::LiteKernel *Scheduler::ScheduleNode(const std::vector<Tensor *> &in_tensors,
                                            const std::vector<Tensor *> &out_tensors,
                                            const mindspore::lite::PrimitiveC *primitive, const Model::Node *node) {
  MS_ASSERT(primitive != nullptr);
  TypeId data_type = GetFirstFp32Fp16OrInt8Type(in_tensors);
  kernel::KernelKey desc{kernel::KERNEL_ARCH::kCPU, data_type, static_cast<schema::PrimitiveType>(primitive->Type())};
#if SUPPORT_GPU
  if (context_->device_type_ == DT_GPU) {
    desc.arch = kernel::KERNEL_ARCH::kGPU;
    auto *kernel = KernelRegistry::GetInstance()->GetKernel(in_tensors, out_tensors, primitive, context_, desc);
    if (kernel != nullptr) {
      kernel->set_desc(desc);
      return kernel;
    } else {
      MS_LOG(ERROR) << "Not supported GPU Op "
                    << schema::EnumNamePrimitiveType(static_cast<schema::PrimitiveType>(primitive->Type())) << " "
                    << node->name_;
    }
  }
#endif
  desc.arch = kernel::KERNEL_ARCH::kCPU;
  kernel::LiteKernel *kernel = nullptr;
  if ((context_->float16_priority && data_type == kNumberTypeFloat32) || data_type == kNumberTypeFloat16) {
    // check if support fp16
    kernel::KernelKey key{desc.arch, kNumberTypeFloat16, desc.type};
    kernel = KernelRegistry::GetInstance()->GetKernel(in_tensors, out_tensors, primitive, context_, key);
    if (kernel != nullptr) {
      MS_LOG(INFO) << "Get fp16 op success. type:"
                   << schema::EnumNamePrimitiveType(static_cast<schema::PrimitiveType>(primitive->Type()));
      desc.data_type = kNumberTypeFloat16;
      kernel->set_desc(desc);
      return kernel;
    }
    MS_LOG(DEBUG) << "Get fp16 op failed, back to fp32 op.";
  }
  if (data_type == kNumberTypeFloat16) {
    desc.data_type = kNumberTypeFloat32;
  }
  kernel = KernelRegistry::GetInstance()->GetKernel(in_tensors, out_tensors, primitive, context_, desc);
  if (kernel != nullptr) {
    kernel->set_desc(desc);
    return kernel;
  }
  return nullptr;
}

TypeId Scheduler::GetFirstFp32Fp16OrInt8Type(const std::vector<Tensor *> &in_tensors) {
  for (const auto &tensor : in_tensors) {
    auto dtype = tensor->data_type();
    if (dtype == kNumberTypeFloat32 || dtype == kNumberTypeFloat16 || dtype == kNumberTypeInt8) {
      return dtype;
    }
  }
  return kNumberTypeFloat32;
}

void Scheduler::SetKernelTensorDataType(kernel::LiteKernel *kernel) {
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
      if (tensor->category() != Tensor::Category::CONST && tensor->data_type() == kNumberTypeFloat16) {
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

}  // namespace mindspore::lite
