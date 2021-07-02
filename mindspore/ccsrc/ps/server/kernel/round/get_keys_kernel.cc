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

#include "ps/server/kernel/round/get_keys_kernel.h"
#include <vector>
#include <memory>

namespace mindspore {
namespace ps {
namespace server {
namespace kernel {
void GetKeysKernel::InitKernel(size_t) {
  if (LocalMetaStore::GetInstance().has_value(kCtxTotalTimeoutDuration)) {
    iteration_time_window_ = LocalMetaStore::GetInstance().value<size_t>(kCtxTotalTimeoutDuration);
  }

  executor_ = &Executor::GetInstance();
  MS_EXCEPTION_IF_NULL(executor_);
  if (!executor_->initialized()) {
    MS_LOG(EXCEPTION) << "Executor must be initialized in server pipeline.";
    return;
  }

  cipher_key_ = &armour::CipherKeys::GetInstance();
}

bool GetKeysKernel::Launch(const std::vector<AddressPtr> &inputs, const std::vector<AddressPtr> &workspace,
                           const std::vector<AddressPtr> &outputs) {
  std::shared_ptr<server::FBBuilder> fbb = std::make_shared<server::FBBuilder>();
  bool response = false;
  size_t iter_num = LocalMetaStore::GetInstance().curr_iter_num();
  size_t total_duration = LocalMetaStore::GetInstance().value<size_t>(kCtxTotalTimeoutDuration);
  MS_LOG(INFO) << "ITERATION NUMBER IS : " << iter_num << ", Total GetKeysKernel allowed Duration Is "
               << total_duration;
  clock_t start_time = clock();

  if (inputs.size() != 1) {
    MS_LOG(ERROR) << "GetKeysKernel needs 1 input,but got " << inputs.size();
    cipher_key_->BuildGetKeys(fbb, schema::ResponseCode_SystemError, iter_num,
                              std::to_string(CURRENT_TIME_MILLI.count()), false);
  } else if (outputs.size() != 1) {
    MS_LOG(ERROR) << "GetKeysKernel needs 1 output,but got " << outputs.size();
    cipher_key_->BuildGetKeys(fbb, schema::ResponseCode_SystemError, iter_num,
                              std::to_string(CURRENT_TIME_MILLI.count()), false);
  } else {
    if (DistributedCountService::GetInstance().CountReachThreshold(name_)) {
      MS_LOG(ERROR) << "Current amount for GetKeysKernel is enough.";
      cipher_key_->BuildGetKeys(fbb, schema::ResponseCode_OutOfTime, iter_num,
                                std::to_string(CURRENT_TIME_MILLI.count()), false);
    } else {
      void *req_data = inputs[0]->addr;
      const schema::GetExchangeKeys *get_exchange_keys_req = flatbuffers::GetRoot<schema::GetExchangeKeys>(req_data);
      int32_t iter_client = (size_t)get_exchange_keys_req->iteration();
      if (iter_num != (size_t)iter_client) {
        MS_LOG(ERROR) << "GetKeysKernel iteration invalid. server now iteration is " << iter_num
                      << ". client request iteration is " << iter_client;
        cipher_key_->BuildGetKeys(fbb, schema::ResponseCode_OutOfTime, iter_num,
                                  std::to_string(CURRENT_TIME_MILLI.count()), false);
      } else {
        response =
          cipher_key_->GetKeys(iter_num, std::to_string(CURRENT_TIME_MILLI.count()), get_exchange_keys_req, fbb);
        if (response) {
          DistributedCountService::GetInstance().Count(name_, get_exchange_keys_req->fl_id()->str());
        }
      }
    }
  }
  GenerateOutput(outputs, fbb->GetCurrentBufferPointer(), fbb->GetSize());
  clock_t end_time = clock();
  double duration = static_cast<double>((end_time - start_time) * 1.0 / CLOCKS_PER_SEC);
  MS_LOG(INFO) << "GetKeysKernel DURATION TIME IS : " << duration;
  return response;
}

bool GetKeysKernel::Reset() {
  MS_LOG(INFO) << "get keys kernel reset! ITERATION NUMBER IS : " << LocalMetaStore::GetInstance().curr_iter_num();
  cipher_key_->ClearKeys();
  DistributedCountService::GetInstance().ResetCounter(name_);
  StopTimer();
  return true;
}

REG_ROUND_KERNEL(getKeys, GetKeysKernel)
}  // namespace kernel
}  // namespace server
}  // namespace ps
}  // namespace mindspore
