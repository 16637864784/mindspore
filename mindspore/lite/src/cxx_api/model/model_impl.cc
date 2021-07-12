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

#include "src/cxx_api/model/model_impl.h"
#include <memory>
#include <algorithm>
#include "include/api/types.h"
#include "include/api/context.h"
#include "include/lite_session.h"
#include "include/context.h"
#include "src/runtime/inner_allocator.h"
#include "src/cxx_api/converters.h"
#include "src/cxx_api/graph/graph_data.h"
#include "src/cxx_api/tensor/tensor_impl.h"
#include "src/cxx_api/tensor_utils.h"
#include "src/common/log_adapter.h"
#include "src/train/train_session.h"

namespace mindspore {
using mindspore::lite::RET_ERROR;
using mindspore::lite::RET_OK;

CreateTrainSessionProto *CreateTrainSessionCallbackHolder(CreateTrainSessionProto *proto) {
  static CreateTrainSessionProto *proto_ = nullptr;
  if (proto != nullptr) {
    proto_ = proto;
  }
  return proto_;
}

Status ModelImpl::Build(const void *model_data, size_t data_size, ModelType model_type,
                        const std::shared_ptr<Context> &ms_context) {
  context_ = ms_context;
  lite::Context lite_context;
  auto status = A2L_ConvertContext(ms_context.get(), &lite_context);
  if (status != kSuccess) {
    return status;
  }

  auto session = std::shared_ptr<session::LiteSession>(
    session::LiteSession::CreateSession(static_cast<const char *>(model_data), data_size, &lite_context));
  if (session == nullptr) {
    MS_LOG(ERROR) << "Allocate session failed.";
    return kLiteNullptr;
  }

  session_.swap(session);
  MS_LOG(DEBUG) << "Build model success.";
  return kSuccess;
}

Status ModelImpl::Build() {
  MS_LOG(DEBUG) << "Start build model.";
  if (graph_ == nullptr || graph_->graph_data_ == nullptr) {
    MS_LOG(ERROR) << "Invalid graph.";
    return kLiteNullptr;
  }

  if (context_ == nullptr) {
    MS_LOG(ERROR) << "Invalid context.";
    return kLiteNullptr;
  }

  lite::Context model_context;
  auto status = A2L_ConvertContext(context_.get(), &model_context);
  if (status != kSuccess) {
    MS_LOG(ERROR) << "Failed to convert Context to Lite Context";
    return status;
  }

  auto create_callback = CreateTrainSessionCallbackHolder();
  if (create_callback != nullptr) {
    auto session = create_callback(graph_->graph_data_, cfg_, &model_context);
    if (session != nullptr) {
      session_ = session;
      MS_LOG(DEBUG) << "Build model success.";
      return kSuccess;
    }
  }

  auto model = graph_->graph_data_->lite_model();
  if (model == nullptr || model->buf == nullptr) {
    MS_LOG(ERROR) << "Lite model has been freed.";
    return kLiteError;
  }
  auto session = std::shared_ptr<session::LiteSession>(session::LiteSession::CreateSession(&model_context));
  if (session == nullptr) {
    MS_LOG(ERROR) << "Allocate session failed.";
    return kLiteNullptr;
  }
  auto ret = session->CompileGraph(model.get());
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "Build model failed.";
    return static_cast<StatusCode>(ret);
  }
  session_.swap(session);
  model->Free();
  MS_LOG(DEBUG) << "Build model success.";
  return kSuccess;
}

static void ResetTensorData(std::vector<void *> old_data, std::vector<tensor::MSTensor *> tensors) {
  for (size_t j = 0; j < old_data.size(); j++) {
    tensors.at(j)->set_data(old_data.at(j));
  }
}

Status ModelImpl::RunGraph(const MSKernelCallBack &before, const MSKernelCallBack &after) {
  if (before == nullptr || after == nullptr) {
    auto ret = session_->RunGraph();
    return static_cast<StatusCode>(ret);
  }
  auto before_call_back = [&](const std::vector<mindspore::tensor::MSTensor *> &before_inputs,
                              const std::vector<mindspore::tensor::MSTensor *> &before_outputs,
                              const CallBackParam &call_param) {
    std::vector<MSTensor> inputs = LiteTensorsToMSTensors(before_inputs);
    std::vector<MSTensor> outputs = LiteTensorsToMSTensors(before_outputs);
    MSCallBackParam mscall_param;
    mscall_param.node_name_ = call_param.node_name;
    mscall_param.node_type_ = call_param.node_type;
    return before(inputs, outputs, mscall_param);
  };

  auto after_call_back = [&](const std::vector<mindspore::tensor::MSTensor *> &before_inputs,
                             const std::vector<mindspore::tensor::MSTensor *> &before_outputs,
                             const CallBackParam &call_param) {
    std::vector<MSTensor> inputs = LiteTensorsToMSTensors(before_inputs);
    std::vector<MSTensor> outputs = LiteTensorsToMSTensors(before_outputs);
    MSCallBackParam mscall_param;
    mscall_param.node_name_ = call_param.node_name;
    mscall_param.node_type_ = call_param.node_type;
    return after(inputs, outputs, mscall_param);
  };
  auto ret = session_->RunGraph(before_call_back, after_call_back);
  return static_cast<StatusCode>(ret);
}

bool ModelImpl::IsTrainModel() { return (graph_ && graph_->graph_data_ && graph_->graph_data_->IsTrainModel()); }

Status ModelImpl::Predict(const std::vector<MSTensor> &inputs, std::vector<MSTensor> *outputs,
                          const MSKernelCallBack &before, const MSKernelCallBack &after) {
  if (outputs == nullptr) {
    MS_LOG(ERROR) << "outputs is nullptr.";
    return kLiteError;
  }
  if (session_ == nullptr) {
    MS_LOG(ERROR) << "Run graph failed.";
    return kLiteError;
  }
  auto input_tensors = session_->GetInputs();
  if (input_tensors.empty()) {
    MS_LOG(ERROR) << "Failed to get input tensor.";
    return kLiteError;
  }
  if (input_tensors.size() != inputs.size()) {
    MS_LOG(ERROR) << "Wrong input size.";
    return kLiteError;
  }
  std::vector<void *> old_data;
  for (size_t i = 0; i < inputs.size(); i++) {
    auto input = input_tensors.at(i);
    auto user_input = inputs.at(i);
    if (user_input.DataType() != static_cast<enum DataType>(input->data_type())) {
      ResetTensorData(old_data, input_tensors);
      MS_LOG(ERROR) << "Tensor " << user_input.Name() << " has a different data type from input" << input->tensor_name()
                    << ".";
      return kLiteInputTensorError;
    }
    if (user_input.Data() == nullptr) {
      ResetTensorData(old_data, input_tensors);
      MS_LOG(ERROR) << "Tensor " << user_input.Name() << " has no data.";
      return kLiteInputTensorError;
    }
    if (user_input.Name() != input->tensor_name()) {
      MS_LOG(WARNING) << "Tensor " << user_input.Name() << " has a different name from input" << input->tensor_name()
                      << ".";
    }
    old_data.push_back(input->data());
    if (input->data_type() == kObjectTypeString) {
      std::vector<int32_t> shape = TruncateShape(user_input.Shape(), input->data_type(), user_input.DataSize(), false);
      if (shape.empty() && !(user_input.Shape().empty())) {
        ResetTensorData(old_data, input_tensors);
        MS_LOG(ERROR) << "Input dims of tensor " << user_input.Name() << " is invalid.";
        return kLiteParamInvalid;
      }
      input->set_shape(shape);
      input->set_data(user_input.MutableData());
    } else {
      if (user_input.MutableData() != input->data()) {
        if (input->Size() != user_input.DataSize()) {
          ResetTensorData(old_data, input_tensors);
          MS_LOG(ERROR) << "Tensor " << user_input.Name() << " has wrong data size.";
          return kLiteInputTensorError;
        }
        input->set_data(user_input.MutableData());
      }
    }
  }
  auto ret = RunGraph(before, after);
  ResetTensorData(old_data, input_tensors);
  if (ret != kSuccess) {
    MS_LOG(ERROR) << "Run graph failed.";
    return ret;
  }
  MS_LOG(DEBUG) << "Run graph success.";
  auto res = GetOutputs();
  if (res.empty()) {
    MS_LOG(DEBUG) << "Empty outputs.";
    return kLiteError;
  }
  outputs->clear();
  outputs->insert(outputs->end(), res.begin(), res.end());
  return kSuccess;
}

std::vector<MSTensor> ModelImpl::GetInputs() {
  std::vector<MSTensor> empty;
  if (session_ == nullptr) {
    MS_LOG(ERROR) << "Session is null.";
    return empty;
  }
  std::vector<MSTensor> res;
  auto inputs = session_->GetInputs();
  if (inputs.empty()) {
    MS_LOG(ERROR) << "The inputs of model is null.";
    return empty;
  }
  res.resize(inputs.size());
  for (size_t i = 0; i < inputs.size(); i++) {
    inputs[i]->MutableData();  // prepare data
    auto impl = std::shared_ptr<MSTensor::Impl>(new (std::nothrow) MSTensor::Impl(inputs[i]));
    if (impl == nullptr || impl->lite_tensor() == nullptr) {
      MS_LOG(ERROR) << "Create tensor failed.";
      return empty;
    }
    auto tensor = MSTensor(impl);
    if (tensor == nullptr) {
      MS_LOG(ERROR) << "Create tensor failed.";
      return empty;
    }
    res[i] = tensor;
  }
  return res;
}

std::vector<MSTensor> ModelImpl::GetOutputs() {
  std::vector<MSTensor> empty;
  if (session_ == nullptr) {
    MS_LOG(ERROR) << "Session is null.";
    return empty;
  }
  std::vector<MSTensor> res;
  auto names = session_->GetOutputTensorNames();
  if (names.empty()) {
    MS_LOG(ERROR) << "The names of model is null.";
    return empty;
  }
  auto outputs = session_->GetOutputs();
  if (outputs.empty()) {
    MS_LOG(ERROR) << "The outputs of model is null.";
    return empty;
  }
  if (names.size() != outputs.size()) {
    MS_LOG(ERROR) << "The size of outputs dose not match the size of names.";
    return empty;
  }
  res.resize(names.size());
  for (size_t i = 0; i < names.size(); i++) {
    auto impl = std::shared_ptr<MSTensor::Impl>(new (std::nothrow) MSTensor::Impl(outputs[names[i]]));
    if (impl == nullptr || impl->lite_tensor() == nullptr) {
      MS_LOG(ERROR) << "Create tensor failed.";
      return empty;
    }
    auto tensor = MSTensor(impl);
    if (tensor == nullptr) {
      MS_LOG(ERROR) << "Create tensor failed.";
      return empty;
    }
    res[i] = tensor;
  }
  return res;
}

MSTensor ModelImpl::GetInputByTensorName(const std::string &name) {
  if (session_ == nullptr) {
    MS_LOG(ERROR) << "Session is null.";
    return MSTensor(nullptr);
  }
  auto res = session_->GetInputsByTensorName(name);
  if (res == nullptr) {
    MS_LOG(ERROR) << "Model does not contains tensor " << name << " .";
    return MSTensor(nullptr);
  }
  auto impl = std::shared_ptr<MSTensor::Impl>(new (std::nothrow) MSTensor::Impl(res));
  if (impl == nullptr || impl->lite_tensor() == nullptr) {
    MS_LOG(ERROR) << "Create tensor failed.";
    return MSTensor(nullptr);
  }

  return MSTensor(impl);
}

std::vector<std::string> ModelImpl::GetOutputTensorNames() {
  if (session_ == nullptr) {
    MS_LOG(ERROR) << "Session is null.";
    std::vector<std::string> empty;
    return empty;
  }
  return session_->GetOutputTensorNames();
}

MSTensor ModelImpl::GetOutputByTensorName(const std::string &name) {
  if (session_ == nullptr) {
    MS_LOG(ERROR) << "Session is null.";
    return MSTensor(nullptr);
  }
  auto res = session_->GetOutputByTensorName(name);
  if (res == nullptr) {
    MS_LOG(ERROR) << "Model does not contains tensor " << name << " .";
    return MSTensor(nullptr);
  }
  auto impl = std::shared_ptr<MSTensor::Impl>(new (std::nothrow) MSTensor::Impl(res));
  if (impl == nullptr || impl->lite_tensor() == nullptr) {
    MS_LOG(ERROR) << "Create tensor failed.";
    return MSTensor(nullptr);
  }

  return MSTensor(impl);
}

std::vector<MSTensor> ModelImpl::GetOutputsByNodeName(const std::string &name) {
  std::vector<MSTensor> empty;
  if (session_ == nullptr) {
    MS_LOG(ERROR) << "Session is null.";
    return empty;
  }
  std::vector<MSTensor> res;
  auto outputs = session_->GetOutputsByNodeName(name);
  if (outputs.empty()) {
    MS_LOG(ERROR) << "The outputs of model is null.";
    return empty;
  }
  res.resize(outputs.size());
  for (size_t i = 0; i < outputs.size(); i++) {
    auto impl = std::shared_ptr<MSTensor::Impl>(new (std::nothrow) MSTensor::Impl(outputs[i]));
    if (impl == nullptr || impl->lite_tensor() == nullptr) {
      MS_LOG(ERROR) << "Create tensor failed.";
      return empty;
    }
    auto tensor = MSTensor(impl);
    if (tensor == nullptr) {
      MS_LOG(ERROR) << "Create tensor failed.";
      return empty;
    }
    res[i] = tensor;
  }
  return res;
}

Status ModelImpl::Resize(const std::vector<MSTensor> &inputs, const std::vector<std::vector<int64_t>> &dims) {
  if (session_ == nullptr) {
    MS_LOG(ERROR) << "Session is null.";
    return kLiteNullptr;
  }
  if (inputs.empty()) {
    MS_LOG(ERROR) << "Inputs is null.";
    return kLiteInputParamInvalid;
  }
  if (dims.empty()) {
    MS_LOG(ERROR) << "Dims is null.";
    return kLiteInputParamInvalid;
  }
  if (inputs.size() != dims.size()) {
    MS_LOG(ERROR) << "The size of inputs does not match the size of dims.";
    return kLiteInputParamInvalid;
  }
  auto model_inputs = session_->GetInputs();
  if (model_inputs.empty()) {
    MS_LOG(ERROR) << "The inputs of model is null.";
    return kLiteParamInvalid;
  }
  if (inputs.size() != model_inputs.size()) {
    MS_LOG(ERROR) << "The size of inputs is incorrect.";
    return kLiteInputParamInvalid;
  }
  std::vector<tensor::MSTensor *> inner_input;
  inner_input.resize(inputs.size());
  std::vector<std::vector<int32_t>> truncated_shape;
  truncated_shape.resize(inputs.size());
  for (size_t i = 0; i < inputs.size(); i++) {
    auto input = inputs[i];
    if (input.impl_ == nullptr || input.impl_->lite_tensor() == nullptr) {
      MS_LOG(ERROR) << "Input tensor " << input.Name() << " is null.";
      return kLiteInputTensorError;
    }
    inner_input[i] = input.impl_->lite_tensor();
    std::vector<int32_t> shape = TruncateShape(dims[i], inner_input[i]->data_type(), inner_input[i]->Size(), false);
    if (shape.empty() && !(dims[i].empty())) {
      MS_LOG(ERROR) << "Input dims[" << i << "] is invalid.";
      return kLiteParamInvalid;
    }
    truncated_shape[i] = shape;
  }
  auto ret = session_->Resize(inner_input, truncated_shape);
  return static_cast<StatusCode>(ret);
}

}  // namespace mindspore
