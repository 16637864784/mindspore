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

#include "ps/core/communicator/communicator_base.h"
#include <memory>

namespace mindspore {
namespace ps {
namespace core {
bool CommunicatorBase::SendResponse(const void *rsp_data, size_t rsp_len, std::shared_ptr<MessageHandler> msg_handler) {
  // The rsp_len could be 0 because of ProtoBuffer's feature.
  if (rsp_data == nullptr || msg_handler == nullptr) {
    MS_LOG(ERROR) << "SendResponse inputs are invalid.";
    return false;
  }
  return msg_handler->SendResponse(rsp_data, rsp_len);
}
void CommunicatorBase::Join() {
  if (!running_thread_.joinable()) {
    MS_LOG(WARNING) << "The running thread of communicator is not joinable.";
    return;
  }
  running_thread_.join();
  return;
}
}  // namespace core
}  // namespace ps
}  // namespace mindspore
