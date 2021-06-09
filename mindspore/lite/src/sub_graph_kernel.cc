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

#include "src/sub_graph_kernel.h"
#include "src/tensor.h"
#include "src/tensorlist.h"
#ifdef ENABLE_FP16
#include "src/runtime/kernel/arm/fp16/fp16_op_handler.h"
#endif
#include "src/common/version_manager.h"
#include "src/runtime/infer_manager.h"
#include "src/common/tensor_util.h"
#include "src/common/utils.h"

namespace mindspore::kernel {
using mindspore::lite::RET_ERROR;
using mindspore::lite::RET_INFER_ERR;
using mindspore::lite::RET_INFER_INVALID;
using mindspore::lite::RET_OK;

int SubGraphKernel::Prepare() {
  for (auto node : this->nodes_) {
    if (node == nullptr) {
      MS_LOG(ERROR) << "node in Subgraph is nullptr";
      return mindspore::lite::RET_NULL_PTR;
    }
    auto ret = node->Prepare();
    if (ret != RET_OK) {
      MS_LOG(ERROR) << "prepare node " << node->name() << " failed";
      return ret;
    }
  }
  return RET_OK;
}

std::string SubGraphKernel::ToString() const {
  std::ostringstream oss;
  oss << "===============================================" << std::endl << "Subgraph type : " << this->subgraph_type_;
  oss << std::endl << this->in_tensors().size() << "Subgraph inputTensors:";
  for (auto tensor : in_tensors()) {
    oss << " " << tensor;
  }
  oss << std::endl << this->out_tensors().size() << "Subgraph outputTensors:";
  for (auto tensor : out_tensors()) {
    oss << " " << tensor;
  }
  oss << std::endl << "Subgraph input nodes :" << std::endl;
  for (auto kernel : this->in_nodes_) {
    oss << " " << kernel->ToString() << std::endl;
  }
  oss << std::endl << "Subgraph output nodes :" << std::endl;
  for (auto kernel : this->out_nodes_) {
    oss << " " << kernel->ToString() << std::endl;
  }
  oss << std::endl << nodes_.size() << "　nodes in subgraph :";
  for (auto kernel : this->nodes_) {
    oss << " " << kernel->name();
  }
  return oss.str();
}

int SubGraphKernel::Execute(const KernelCallBack &before, const KernelCallBack &after) {
  if (this->executor_ == nullptr) {
    MS_LOG(ERROR) << "executor is nullptr";
    return RET_ERROR;
  }
  auto ret = executor_->Run(this->in_tensors(), this->out_tensors(), this->nodes_, this->Context()->allocator.get(),
                            before, after);
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "Run sub graph failed: " << ret;
    return ret;
  }

  return lite::RET_OK;
}

int SubGraphKernel::ReSize() {
  for (auto kernel : nodes_) {
    if (kernel == nullptr) {
      MS_LOG(ERROR) << "input kernel is nullptr!";
      return RET_ERROR;
    }
    if (kernel->subgraph_type() != kernel::kNotSubGraph) {
      MS_LOG(ERROR) << "all nodes in should be kernel";
      return RET_ERROR;
    }
    std::vector<lite::Tensor *> inputs = kernel->in_tensors();
    std::vector<lite::Tensor *> outputs = kernel->out_tensors();
    for (auto &output : outputs) {
      output->FreeData();
    }
    auto ret =
      lite::KernelInferShape(inputs, outputs, kernel->kernel()->primitive(),
                             static_cast<const lite::InnerContext *>(kernel->kernel()->context())->GetProviders());
    if (ret == lite::RET_NOT_SUPPORT) {
      auto parameter = kernel->op_parameter();
      if (parameter == nullptr) {
        MS_LOG(ERROR) << "kernel(" << kernel->name() << ")'s op_parameter is nullptr!";
        return RET_ERROR;
      }
      ret = lite::KernelInferShape(inputs, outputs, parameter);
    }

    if (ret == RET_INFER_INVALID) {
      MS_LOG(INFO) << "InferShape shouldn't be done before runtime, type:"
                   << schema::EnumNamePrimitiveType(static_cast<schema::PrimitiveType>(kernel->type()))
                   << "flag set to false.";
    } else if (ret != RET_OK) {
      MS_LOG(ERROR) << "InferShape failed, type: "
                    << schema::EnumNamePrimitiveType(static_cast<schema::PrimitiveType>(kernel->type()));
      return RET_INFER_ERR;
    }
    if (ret == RET_OK) {
      ret = kernel->ReSize();
      if (ret != RET_OK) {
        MS_LOG(ERROR) << "kernel " << kernel->name() << " resize fail!ret = " << ret;
        return ret;
      }
    }
  }
  return RET_OK;
}

void SubGraphKernel::InitOutTensorInitRefCount() {
  for (auto *node : nodes_) {
    node->InitOutTensorInitRefCount();
  }
}
void SubGraphKernel::DropNode(LiteKernel *node) {
  lite::VectorErase(&nodes_, node);
  lite::VectorErase(&in_nodes_, node);
  lite::VectorErase(&out_nodes_, node);
}

int CustomSubGraph::Prepare() {
  auto ret = SubGraphKernel::Prepare();
  if (ret != RET_OK) {
    return ret;
  }
  if (nodes_.size() < 1) {
    return RET_OK;
  }
  auto provider = nodes_[0]->desc().provider;
  auto context = this->Context();
  AllocatorPtr allocator = nullptr;
  auto iter = std::find_if(context->device_list_.begin(), context->device_list_.end(),
                           [&provider](const auto &dev) { return dev.provider_ == provider; });
  if (iter != context->device_list_.end()) {
    allocator = iter->allocator_;
  }

  for (size_t i = 0; i < nodes_.size() - 1; ++i) {
    auto node = nodes_[i];
    for (auto tensor : node->out_tensors()) {
      MS_ASSERT(tensor != nullptr);
      tensor->set_allocator(allocator);
    }
  }

  auto node = nodes_[nodes_.size() - 1];
  for (auto tensor : node->out_tensors()) {
    MS_ASSERT(tensor != nullptr);
    tensor->set_allocator(this->Context()->allocator);
  }
  return RET_OK;
}

int CustomSubGraph::Execute(const KernelCallBack &before, const KernelCallBack &after) {
  for (auto kernel : nodes_) {
    MS_ASSERT(kernel != nullptr);
    auto ret = kernel->Execute(before, after);
    if (RET_OK != ret) {
      MS_LOG(ERROR) << "run kernel failed, name: " << kernel->name();
      return ret;
    }
  }

  return RET_OK;
}

int CpuSubGraph::Prepare() {
  auto ret = SubGraphKernel::Prepare();
  if (ret != RET_OK) {
    return ret;
  }
  for (auto node : nodes_) {
    for (auto tensor : node->out_tensors()) {
      MS_ASSERT(tensor != nullptr);
      tensor->set_allocator(this->Context()->allocator);
    }
  }
  return RET_OK;
}

int CpuSubGraph::Execute(const KernelCallBack &before, const KernelCallBack &after) {
  MS_ASSERT(nullptr != this->Context()->allocator.get());
#ifdef SUPPORT_GPU
  // In heterogeneous scenarios of CPU and GPU, call MutableData to MapBuffer(synchronize data).
  if (this->Context()->IsGpuEnabled()) {
    for (auto tensor : this->in_tensors()) {
      tensor->MutableData();
    }
  }
#endif

  for (auto *kernel : nodes_) {
    MS_ASSERT(nullptr != kernel);
    auto ret = kernel->Execute(before, after);
    if (RET_OK != ret) {
      MS_LOG(ERROR) << "run kernel failed, name: " << kernel->name();
      return ret;
    }
  }
  return RET_OK;
}
#ifdef ENABLE_FP16
void CpuFp16SubGraph::FreeOriginInputData() {
  for (auto &iter : this->origin_input_data_) {
    auto *data_store = iter.second;
    if (data_store == nullptr) {
      continue;
    }
    // free data in data_store
    if (data_store->data_ != nullptr) {
      if (data_store->allocator_ == nullptr) {
        free(data_store->data_);
      } else {
        data_store->allocator_->Free(data_store->data_);
      }
    }
    // free data_store
    if (this->Context()->allocator != nullptr) {
      this->Context()->allocator->Free(data_store);
    } else {
      free(data_store);
    }
    data_store = nullptr;
  }
  this->origin_input_data_.clear();
}

int CpuFp16SubGraph::Float32TensorToFloat16Tensor(lite::Tensor *tensor) {
  MS_ASSERT(tensor != nullptr);
  auto float32_data = tensor->data_c();
  auto own_data = tensor->own_data();
  tensor->set_data_type(TypeId::kNumberTypeFloat16);
  if (float32_data == nullptr) {
    // the input data may be nullptr of merge.
    MS_LOG(INFO) << "tensor data is null.";
    return lite::RET_OK;
  }
  tensor->set_data(nullptr);
  auto ret = tensor->MallocData();
  if (RET_OK != ret) {
    MS_LOG(ERROR) << "malloc data failed";
    return RET_ERROR;
  }
  MS_ASSERT(tensor->data_c() != nullptr);
  Float32ToFloat16_fp16_handler(float32_data, tensor->data_c(), tensor->ElementsNum());
  if (tensor->allocator() != nullptr) {
    tensor->allocator()->SetRefCount(tensor->data_c(), tensor->allocator()->RefCount(float32_data));
  }
  auto *data_store =
    DataStore::CreateDataStore(float32_data, own_data, tensor->allocator().get(), this->Context()->allocator.get());
  if (data_store == nullptr) {
    MS_LOG(ERROR) << "Create DataStore failed";
    return RET_ERROR;
  }
  origin_input_data_[tensor] = data_store;
  return RET_OK;
}

int CpuFp16SubGraph::Float16TensorToFloat32Tensor(lite::Tensor *tensor) {
  auto float16_data = tensor->data_c();
  if (float16_data == nullptr) {
    MS_LOG(ERROR) << "tensor data is null.";
    return lite::RET_NULL_PTR;
  }
  tensor->set_data(nullptr);
  tensor->set_data_type(TypeId::kNumberTypeFloat32);
  auto ret = tensor->MallocData();
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "malloc data failed";
    if (this->Context() != nullptr && this->Context()->allocator != nullptr) {
      this->Context()->allocator->Free(float16_data);
    } else {
      free(float16_data);
    }
    return RET_ERROR;
  }
  MS_ASSERT(tensor->data_c() != nullptr);
  Float16ToFloat32_fp16_handler(float16_data, tensor->data_c(), tensor->ElementsNum());
  if (tensor->allocator() != nullptr) {
    tensor->allocator()->SetRefCount(tensor->data_c(), tensor->allocator()->RefCount(float16_data));
    tensor->allocator()->Free(float16_data);
  } else {
    free(float16_data);
  }
  return RET_OK;
}

int CpuFp16SubGraph::PreProcess() {
#ifdef ENABLE_FP16
  int ret;
  for (auto tensor : this->in_tensors()) {
    MS_ASSERT(tensor != nullptr);
    auto real_tensor = tensor;
    if (tensor->root_tensor() != nullptr) {
      real_tensor = tensor->root_tensor();
      if (tensor->data_type() == kNumberTypeFloat32) {
        tensor->set_data_type(kNumberTypeFloat16);
      } else if (tensor->data_type() == kObjectTypeTensorType) {
        auto tensorlist = reinterpret_cast<lite::TensorList *>(tensor);
        if (tensorlist->tensors_data_type() == kNumberTypeFloat32) {
          tensorlist->set_tensors_data_type(kNumberTypeFloat16);
        }
      }
    }
    if (real_tensor->data_type() == kNumberTypeFloat32) {
      ret = Float32TensorToFloat16Tensor(real_tensor);
      if (RET_OK != ret) {
        MS_LOG(ERROR) << "Float32TensorToFloat16Tensor failed.";
        this->FreeOriginInputData();
        return ret;
      }
    } else if (real_tensor->data_type() == kObjectTypeTensorType) {
      auto tensorlist = reinterpret_cast<lite::TensorList *>(real_tensor);
      if (tensorlist->tensors_data_type() == kNumberTypeFloat32) {
        tensorlist->set_tensors_data_type(kNumberTypeFloat16);
        for (auto inner_tensor : tensorlist->tensors()) {
          ret = Float32TensorToFloat16Tensor(inner_tensor);
          if (RET_OK != ret) {
            MS_LOG(ERROR) << "Float32TensorToFloat16Tensor failed.";
            this->FreeOriginInputData();
            return ret;
          }
        }
      }
    }
  }
  for (auto kernel : this->nodes_) {
    for (auto tensor : kernel->out_tensors()) {
      if (kernel->type() == schema::PrimitiveType_Cast) {
        continue;
      }
      if (tensor->data_type() == kNumberTypeFloat32) {
        tensor->set_data_type(kNumberTypeFloat16);
      } else if (tensor->data_type() == kObjectTypeTensorType) {
        auto tensorlist = reinterpret_cast<lite::TensorList *>(tensor);
        if (tensorlist->tensors_data_type() == kNumberTypeFloat32) {
          tensorlist->set_tensors_data_type(kNumberTypeFloat16);
        }
      }
    }
  }
  return RET_OK;
#else
  return RET_OK;
#endif
}

int CpuFp16SubGraph::PostProcess() {
#ifdef ENABLE_FP16
  int ret;
  for (auto tensor : this->out_tensors()) {
    MS_ASSERT(tensor != nullptr);
    if (tensor->data_type() == kNumberTypeFloat16) {
      ret = Float16TensorToFloat32Tensor(tensor);
      if (RET_OK != ret) {
        MS_LOG(ERROR) << "Float16TensorToFloat32Tensor failed.";
        return ret;
      }
    } else if (tensor->data_type() == kObjectTypeTensorType) {
      auto tensorlist = reinterpret_cast<lite::TensorList *>(tensor);
      if (tensorlist->tensors_data_type() == kNumberTypeFloat16) {
        tensorlist->set_tensors_data_type(kNumberTypeFloat32);
        for (auto inner_tensor : tensorlist->tensors()) {
          ret = Float16TensorToFloat32Tensor(inner_tensor);
          if (RET_OK != ret) {
            MS_LOG(ERROR) << "Float32TensorToFloat16Tensor failed.";
            return ret;
          }
        }
      }
    }
  }

  int tensor_count = 0;
  auto in_tensors = this->in_tensors();
  for (size_t i = 0; i < in_tensors.size(); i++) {
    auto tensor = in_tensors.at(i);
    MS_ASSERT(tensor != nullptr);
    auto real_tensor = tensor;
    if (tensor->root_tensor() != nullptr) {
      real_tensor = tensor->root_tensor();
      if (tensor->data_type() == kNumberTypeFloat16) {
        tensor->set_data_type(kNumberTypeFloat32);
      } else if (tensor->data_type() == kObjectTypeTensorType) {
        auto tensorlist = reinterpret_cast<lite::TensorList *>(tensor);
        if (tensorlist->tensors_data_type() == kNumberTypeFloat16) {
          tensorlist->set_tensors_data_type(kNumberTypeFloat32);
        }
      }
    }
    if (real_tensor->data_type() == kNumberTypeFloat16 &&
        origin_input_data_.find(real_tensor) != origin_input_data_.end()) {
      auto origin_tensor_data = origin_input_data_.at(real_tensor);
      real_tensor->FreeData();
      MS_ASSERT(origin_tensor_data->data_ != nullptr);
      real_tensor->set_data(origin_tensor_data->data_);
      real_tensor->set_own_data(origin_tensor_data->own_data_);
      real_tensor->set_data_type(kNumberTypeFloat32);
      origin_tensor_data->data_ = nullptr;
      tensor_count++;
    } else if (real_tensor->data_type() == kObjectTypeTensorType) {
      auto tensorlist = reinterpret_cast<lite::TensorList *>(real_tensor);
      if (tensorlist->tensors_data_type() == kNumberTypeFloat16) {
        tensorlist->set_tensors_data_type(kNumberTypeFloat32);
        for (auto inner_tensor : tensorlist->tensors()) {
          MS_ASSERT(inner_tensor != nullptr);
          auto origin_tensor_data = origin_input_data_.at(inner_tensor);
          inner_tensor->FreeData();
          MS_ASSERT(origin_tensor_data->data_ != nullptr);
          inner_tensor->set_data(origin_tensor_data->data_);
          inner_tensor->set_own_data(origin_tensor_data->own_data_);
          inner_tensor->set_data_type(kNumberTypeFloat32);
          origin_tensor_data->data_ = nullptr;
          tensor_count++;
        }
      }
    }
  }
  this->FreeOriginInputData();
  return RET_OK;
#else
  return RET_OK;
#endif
}
#endif
}  // namespace mindspore::kernel
