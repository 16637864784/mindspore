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

#include "runtime/framework/actor/data_source_actor.h"
#include "runtime/framework/actor/kernel_actor.h"
#include "runtime/framework/actor/memory_manager_actor.h"
#include "runtime/framework/actor/output_actor.h"
#include "mindrt/include/async/async.h"
#include "common/trans.h"
#include "utils/log_adapter.h"

namespace mindspore {
namespace runtime {
void DataSourceActor::Init() {
  // Init output data.
  for (auto &data_arrow : output_data_arrows_) {
    MS_EXCEPTION_IF_NULL(data_arrow);
    auto data = std::make_unique<OpData<DeviceTensor>>(data_arrow->to_op_id_, nullptr, data_arrow->to_input_index_);
    output_data_.emplace_back(std::move(data));
  }
}

void DataSourceActor::FetchData(OpContext<DeviceTensor> *context) {
  MS_LOG(INFO) << "Data source actor(" << GetAID().Name() << ") fetches data.";
  MS_EXCEPTION_IF_NULL(context);
  // Pop the data of last time.
  if (!buffers_.empty()) {
    buffers_.pop();
  }

  // Construct device tensors and fill to the buffers from member nodes.
  FillDataBuffer();
  if (buffers_.size() == 0) {
    SET_OPCONTEXT_FAIL_RET_WITH_ERROR((*context), "The data queue is empty.");
  }

  // Allocate memory for device tensors.
  SendMemoryAllocReq(context);
}

void DataSourceActor::SendOutput(OpContext<DeviceTensor> *context) {
  MS_LOG(INFO) << "Data source actor(" << GetAID().Name() << ") sends output data.";
  MS_EXCEPTION_IF_NULL(context);
  // No output.
  if ((output_data_arrows_.size() == 0) && (output_result_arrows_.size() == 0)) {
    SET_OPCONTEXT_SUCCESS_RET((*context));
  }

  if (buffers_.size() == 0) {
    SET_OPCONTEXT_FAIL_RET_WITH_ERROR((*context), "The data queue is empty.");
  }

  // Send graph output result.
  SendResult(context);

  // Send output data.
  const auto &output_device_tensors = buffers_.front();
  for (size_t i = 0; i < output_data_arrows_.size(); ++i) {
    auto &data_arrow = output_data_arrows_[i];
    auto &output_data = output_data_[i];
    MS_EXCEPTION_IF_NULL(data_arrow);
    MS_EXCEPTION_IF_NULL(output_data);
    if (IntToSize(data_arrow->from_output_index_) >= output_device_tensors.size()) {
      SET_OPCONTEXT_FAIL_RET_WITH_ERROR((*context), "The output index is of range.");
    }
    output_data->data_ = output_device_tensors[data_arrow->from_output_index_];
    Async(data_arrow->to_op_id_, &OpActor::RunOpData, output_data.get(), context);
  }
}

void DeviceQueueDataSourceActor::FillDataBuffer() {
  MS_EXCEPTION_IF_NULL(kernel_info_);
  // Construct device tensors.
  std::vector<DeviceTensor *> device_tensors;
  for (auto &device_tensor : kernel_info_->output_address_list()) {
    MS_EXCEPTION_IF_NULL(device_tensor);
    device_tensors.emplace_back(device_tensor.get());
  }

  buffers_.push(device_tensors);
}

void DeviceQueueDataSourceActor::SendMemoryAllocReq(OpContext<DeviceTensor> *context) {
  auto &device_tensors = buffers_.back();
  Async(memory_manager_aid_, &MemoryManagerActor::AllocateMemory, &device_tensors, device_context_, context, GetAID());
}

void DeviceQueueDataSourceActor::SendMemoryFreeReq(OpContext<DeviceTensor> *context) {
  auto &device_tensors = buffers_.front();
  Async(memory_manager_aid_, &MemoryManagerActor::FreeMemory, &device_tensors, device_context_, context);
}

void DeviceQueueDataSourceActor::OnMemoryAllocFinish(OpContext<DeviceTensor> *context) {
  MS_EXCEPTION_IF_NULL(context);
  MS_EXCEPTION_IF_NULL(device_context_);
  if (buffers_.size() == 0) {
    SET_OPCONTEXT_FAIL_RET_WITH_ERROR((*context), "The data queue is empty.");
  }

  // Construct outputs of data kernel launching.
  auto &device_tensors = buffers_.back();
  std::vector<AddressPtr> kernel_outputs;
  for (auto &device_tensor : device_tensors) {
    MS_EXCEPTION_IF_NULL(device_tensor);
    kernel_outputs.emplace_back(std::make_shared<Address>(device_tensor->GetMutablePtr(), device_tensor->GetSize()));
  }

  // Copy data from device queue by data kernel launching.
  std::vector<AddressPtr> empty_address;
  auto ret = device_context_->LaunchKernel(data_kernel_, empty_address, empty_address, kernel_outputs);
  if (!ret) {
    std::string error_info = "Launch kernel failed: " + data_kernel_->ToString();
    SET_OPCONTEXT_FAIL_RET_WITH_ERROR((*context), error_info);
  }

  // Note that SendMemoryFreeReq must be in front of SendOutput, because SendOutput will trigger SendMemoryAllocReq of
  // the next actor and the actor is asynchronous execution. So it is necessary to ensure that SendMemoryFreeReq of
  // the current actor is in front of SendMemoryAllocReq of the next actor.  One is to reuse the memory more fully,
  // the other is to ensure the execution order and avoid the illegal memory timing problem.
  SendMemoryFreeReq(context);
  SendOutput(context);
}

void DeviceQueueDataSourceActor::SendResult(OpContext<DeviceTensor> *context) {
  for (const auto &result_arrow : output_result_arrows_) {
    MS_EXCEPTION_IF_NULL(result_arrow);
    Async(result_arrow->to_op_id_, &OutputActor::CollectOutput, data_kernel_, result_arrow->from_output_index_,
          result_arrow->to_input_index_, context);
  }
}

void HostQueueDataSourceActor::FillDataBuffer() {
  // Construct device tensors.
  std::vector<DeviceTensor *> device_tensors;
  for (auto &data_node : data_nodes_) {
    auto device_address = AnfAlgo::GetMutableOutputAddr(data_node, 0, false);
    MS_EXCEPTION_IF_NULL(device_address);
    device_tensors.emplace_back(device_address.get());
  }

  buffers_.push(device_tensors);
}

void HostQueueDataSourceActor::SendMemoryAllocReq(OpContext<DeviceTensor> *context) {
  auto &device_tensors = buffers_.back();
  if (IsSameDeviceType()) {
    Async(memory_manager_aid_, &MemoryManagerActor::AllocateMemory, &device_tensors, device_contexts_[0], context,
          GetAID());
  } else {
    Async(memory_manager_aid_, &MemoryManagerActor::AllocateBatchMemory, &device_tensors, &device_contexts_, context,
          GetAID());
  }
}

void HostQueueDataSourceActor::SendMemoryFreeReq(OpContext<DeviceTensor> *context) {
  auto &device_tensors = buffers_.front();
  if (IsSameDeviceType()) {
    Async(memory_manager_aid_, &MemoryManagerActor::FreeMemory, &device_tensors, device_contexts_[0], context);
  } else {
    Async(memory_manager_aid_, &MemoryManagerActor::FreeBatchMemory, &device_tensors, &device_contexts_, context);
  }
}

void HostQueueDataSourceActor::OnMemoryAllocFinish(OpContext<DeviceTensor> *context) {
  MS_EXCEPTION_IF_NULL(context);
  if (buffers_.size() == 0) {
    SET_OPCONTEXT_FAIL_RET_WITH_ERROR((*context), "The data queue is empty.");
  }

  // Get host tensors from host queue and get device tensors from buffers.
  MS_EXCEPTION_IF_NULL(host_queue_);
  if (host_queue_->IsEmpty()) {
    SET_OPCONTEXT_FAIL_RET_WITH_ERROR((*context), "Host data queue is empty.");
  }
  auto &host_tensors = host_queue_->Pull();
  auto &device_tensors = buffers_.back();
  if (host_tensors.size() != device_tensors.size()) {
    SET_OPCONTEXT_FAIL_RET_WITH_ERROR((*context),
                                      "The length of host tensors is not equal to the length of device tensors.");
  }

  // Copy data from host tensor to device tensor.
  for (size_t i = 0; i < host_tensors.size(); ++i) {
    auto &host_tensor = host_tensors[i];
    auto &device_tensor = device_tensors[i];
    MS_EXCEPTION_IF_NULL(host_tensor);
    MS_EXCEPTION_IF_NULL(device_tensor);
    if (!device_tensor->SyncHostToDevice(trans::GetRuntimePaddingShape(data_nodes_[i], 0),
                                         LongToSize(host_tensor->data().nbytes()), host_tensor->data_type(),
                                         host_tensor->data_c(), host_tensor->device_info().host_format_)) {
      SET_OPCONTEXT_FAIL_RET_WITH_ERROR((*context), "SyncHostToDevice failed.");
    }
  }
  host_queue_->Pop();

  // Note that SendMemoryFreeReq must be in front of SendOutput, because SendOutput will trigger SendMemoryAllocReq of
  // the next actor and the actor is asynchronous execution. So it is necessary to ensure that SendMemoryFreeReq of
  // the current actor is in front of SendMemoryAllocReq of the next actor.  One is to reuse the memory more fully,
  // the other is to ensure the execution order and avoid the illegal memory timing problem.
  SendMemoryFreeReq(context);
  SendOutput(context);
}

void HostQueueDataSourceActor::SendResult(OpContext<DeviceTensor> *context) {
  for (const auto &result_arrow : output_result_arrows_) {
    MS_EXCEPTION_IF_NULL(result_arrow);
    if (IntToSize(result_arrow->from_output_index_) >= data_nodes_.size()) {
      SET_OPCONTEXT_FAIL_RET_WITH_ERROR((*context), "The output index is of range.");
    }
    Async(result_arrow->to_op_id_, &OutputActor::CollectOutput, data_nodes_[result_arrow->from_output_index_], 0,
          result_arrow->to_input_index_, context);
  }
}

size_t HostQueueDataSourceActor::FetchDataNodePosition(const AnfNodePtr &data_node) const {
  const auto &iter = data_node_position_map_.find(data_node);
  if (iter == data_node_position_map_.end()) {
    MS_LOG(EXCEPTION) << "Data node: " << data_node->fullname_with_scope() << " is not exist.";
  }
  return iter->second;
}

bool HostQueueDataSourceActor::IsSameDeviceType() const {
  for (size_t i = 1; i < device_contexts_.size(); i++) {
    if (device_contexts_[i] != device_contexts_[0]) {
      return false;
    }
  }
  return true;
}

}  // namespace runtime
}  // namespace mindspore
