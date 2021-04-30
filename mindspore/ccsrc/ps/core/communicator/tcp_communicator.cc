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

#include "ps/core/communicator/tcp_communicator.h"
#include <memory>

namespace mindspore {
namespace ps {
namespace core {
bool TcpCommunicator::Start() {
  if (running_) {
    MS_LOG(INFO) << "The TCP communicator has already started.";
    return true;
  }
  MS_EXCEPTION_IF_NULL(server_node_);

  // Set message callback. For example, message of push/pull, etc.
  tcp_msg_callback_ = std::bind(
    [&](std::shared_ptr<core::TcpConnection> conn, std::shared_ptr<core::MessageMeta> meta, DataPtr data,
        size_t size) -> void {
      TcpUserCommand user_command = static_cast<TcpUserCommand>(meta->user_cmd());
      const std::string &msg_type = kUserCommandToMsgType.at(user_command);
      if (msg_type == "" || !msg_callbacks_[msg_type]) {
        MS_LOG(ERROR) << "Tcp server doesn't support command " << user_command << " " << msg_type;
        return;
      }

      MS_LOG(DEBUG) << "TcpCommunicator receives message for " << msg_type;
      std::shared_ptr<MessageHandler> tcp_msg_handler =
        std::make_shared<TcpMsgHandler>(server_node_, conn, meta, data, size);
      MS_EXCEPTION_IF_NULL(tcp_msg_handler);
      // The Submit function timed out for 30s, if it returns false, it will retry 60 times.
      bool res = CommUtil::Retry([&] { return task_executor_->Submit(msg_callbacks_[msg_type], tcp_msg_handler); },
                                 kRetryCount, kRetryIntervalInMs);
      if (res == false) {
        MS_LOG(EXCEPTION) << "Submit tcp msg handler failed.";
      }

      return;
    },
    std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
  server_node_->set_handler(tcp_msg_callback_);

  // Set event callback. For example, event of scaling out/in, etc.
  event_callback_ = std::bind(
    [&](const core::NodeEvent &event) -> void {
      MS_LOG(INFO) << "Server receives event of " << event;
      certain_event_to_callback_[event]();
    },
    std::placeholders::_1);
  server_node_->set_event_callback(event_callback_);

  server_node_->Start();
  running_ = true;
  running_thread_ = std::thread([&]() {
    while (running_) {
      std::this_thread::yield();
    }
  });
  return true;
}

bool TcpCommunicator::Stop() {
  MS_EXCEPTION_IF_NULL(server_node_);
  server_node_->Finish();
  server_node_->Stop();
  running_ = false;
  return true;
}

void TcpCommunicator::RegisterMsgCallBack(const std::string &msg_type, const MessageCallback &cb) {
  msg_callbacks_.try_emplace(msg_type, cb);
  return;
}

void TcpCommunicator::RegisterEventCallback(const core::NodeEvent &event, const CertainEventCallback &event_cb) {
  certain_event_to_callback_.try_emplace(event, event_cb);
  return;
}

ServerNode *TcpCommunicator::server_node() { return server_node_; }
}  // namespace core
}  // namespace ps
}  // namespace mindspore
