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
#include "src/runtime/infer_manager.h"
#include <algorithm>
#include <set>
#include <string>
#include <memory>
#include "src/common/prim_util.h"
#include "src/common/tensor_util.h"
#include "src/cxx_api/tensor/tensor_impl.h"
#include "schema/model_generated.h"
#include "include/errorcode.h"
#include "nnacl/errorcode.h"
#include "src/tensorlist.h"
#include "include/registry/register_kernel_interface.h"
#include "src/kernel_registry.h"

namespace mindspore {
namespace lite {
#ifdef ENABLE_CUSTOM_KERNEL_REGISTRY
int KernelInferShape(const std::vector<lite::Tensor *> &inputs, const std::vector<lite::Tensor *> &outputs,
                     const void *primitive, std::set<std::string> &&providers) {
  if (primitive == nullptr) {
    return RET_NOT_SUPPORT;
  }
  auto prim_type = GetPrimitiveType(primitive);
  std::shared_ptr<kernel::KernelInterface> kernel_interface = nullptr;
  if (prim_type == schema::PrimitiveType_Custom) {
    kernel_interface =
      registry::RegisterKernelInterface::GetKernelInterface("", static_cast<const schema::Primitive *>(primitive));
  } else {
    for (auto &&provider : providers) {
      kernel_interface = registry::RegisterKernelInterface::GetKernelInterface(
        provider, static_cast<const schema::Primitive *>(primitive));
      if (kernel_interface != nullptr) {
        break;
      }
    }
  }
  if (kernel_interface == nullptr) {
    return RET_NOT_SUPPORT;
  }
  std::vector<mindspore::MSTensor> in_tensors;
  std::transform(inputs.begin(), inputs.end(), std::back_inserter(in_tensors),
                 [](lite::Tensor *tensor) { return mindspore::MSTensor(std::make_shared<MSTensor::Impl>(tensor)); });
  std::vector<mindspore::MSTensor> out_tensors;
  std::transform(outputs.begin(), outputs.end(), std::back_inserter(out_tensors),
                 [](lite::Tensor *tensor) { return mindspore::MSTensor(std::make_shared<MSTensor::Impl>(tensor)); });
  auto ret = kernel_interface->Infer(&in_tensors, &out_tensors, static_cast<const schema::Primitive *>(primitive));
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "op_type: " << PrimitiveTypeName(prim_type) << " infer fail!ret: " << ret;
    return ret;
  }
  return RET_OK;
}
#endif

int KernelInferShape(const std::vector<lite::Tensor *> &inputs, const std::vector<lite::Tensor *> &outputs,
                     OpParameter *parameter) {
  if (inputs.empty()) {
    MS_LOG(ERROR) << "No input!";
    return RET_ERROR;
  }
#ifndef ENABLE_CONTROLFLOW_TENSORLIST
  if (parameter->type_ == schema::PrimitiveType_Switch) {
    MS_LOG(ERROR) << unsupport_controlflow_tensorlist_log;
    return RET_ERROR;
  }
#endif
  std::vector<TensorC *> in_tensors;
  std::vector<TensorC *> out_tensors;
  if (parameter->type_ == schema::PrimitiveType_PartialFusion || parameter->type_ == schema::PrimitiveType_Switch ||
      parameter->type_ == schema::PrimitiveType_Call) {
    MS_LOG(INFO) << "no need infer shape.";
    return RET_OK;
  }

  int ret = GenerateInTensorC(parameter, inputs, outputs, &in_tensors);
  if (ret != RET_OK) {
    FreeAllTensorC(&in_tensors);
    return RET_ERROR;
  }
  ret = GenerateOutTensorC(parameter, inputs, outputs, &out_tensors);
  if (ret != RET_OK) {
    FreeAllTensorC(&in_tensors);
    FreeAllTensorC(&out_tensors);
    return RET_ERROR;
  }
  auto infer_shape_func = GetInferFunc(parameter->type_);
  if (infer_shape_func == nullptr) {
    MS_LOG(ERROR) << "Get infershape func failed! type:" << PrimitiveCurVersionTypeName(parameter->type_);
    return RET_ERROR;
  }
  ret = infer_shape_func(static_cast<TensorC **>(in_tensors.data()), in_tensors.size(), out_tensors.data(),
                         out_tensors.size(), parameter);
  for (size_t i = 0; i < out_tensors.size(); i++) {
    if (out_tensors.at(i) == nullptr) {
      continue;
    }
#ifdef ENABLE_CONTROLFLOW_TENSORLIST
    if (reinterpret_cast<TensorListC *>(out_tensors.at(i))->data_type_ == TypeIdC::kObjectTypeTensorType) {
      auto *tensor_list_c = reinterpret_cast<TensorListC *>(out_tensors.at(i));
      auto *tensor_list = reinterpret_cast<TensorList *>(outputs.at(i));
      tensor_list->set_shape({static_cast<int>(tensor_list_c->element_num_)});
      auto tensor_shape = std::vector<std::vector<int>>(
        tensor_list_c->element_num_,
        std::vector<int>(tensor_list_c->element_shape_,
                         tensor_list_c->element_shape_ + tensor_list_c->element_shape_size_));
      tensor_list->MallocTensorListData(static_cast<TypeId>(tensor_list_c->data_type_), tensor_shape);
      TensorListC2TensorList(tensor_list_c, tensor_list);
    } else {
#endif
      TensorC2Tensor(out_tensors.at(i), outputs.at(i));
#ifdef ENABLE_CONTROLFLOW_TENSORLIST
    }
#endif
    if (ret == NNACL_INFER_INVALID) {
      outputs.at(i)->set_shape({-1});
    }
  }
  FreeAllTensorC(&in_tensors);
  FreeAllTensorC(&out_tensors);
  if (ret == NNACL_INFER_INVALID) {
    return RET_INFER_INVALID;
  } else if (ret != NNACL_OK) {
    if (ret == NNACL_FORMAT_ERROR) {
      MS_LOG(ERROR) << "Unexpected input format " << inputs[0]->format();
    }
    return RET_INFER_ERR;
  }
  return RET_OK;
}
}  // namespace lite
}  // namespace mindspore
