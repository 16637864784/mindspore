/**
 * Copyright 2022 Huawei Technologies Co., Ltd
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

#include "runtime/graph_scheduler/actor/custom_actor.h"
#include "runtime/graph_scheduler/actor/memory_manager_actor.h"
#include "utils/log_adapter.h"
#include "utils/anf_utils.h"

namespace mindspore {
namespace runtime {
void CustomActor::Run(OpContext<DeviceTensor> *const ctx) {
  auto node = kernel_.lock();
  MS_EXCEPTION_IF_NULL(node);
  MS_EXCEPTION_IF_ZERO("device_contexts_ size", device_contexts_.size());
  MS_EXCEPTION_IF_NULL(device_contexts_[0]);
  try {
    auto ret = device_contexts_[0]->LaunchCustomFunc(node);
    if (!ret) {
      std::string error_info = "Launch custom kernel failed: " + node->fullname_with_scope();
      SET_OPCONTEXT_FAIL_RET_WITH_ERROR_BY_STRATEGY(strategy_, (*ctx), error_info);
    }

    // Update the output addr size after inferop && updateop, because after the inferop & updateop, the shape of output
    // maybe changed.
    if (AnfUtils::GetCustomActorType(kernel_.lock()) == kInfer) {
      auto base_node = AnfUtils::GetCustomActorBaseNode(kernel_.lock());
      auto kernel_info = dynamic_cast<KernelInfo *>(base_node->kernel_info());
      UpdateOutputAddrSize(kernel_info, base_node);
      // Update the shape of internal parameter.
      UpdateInternalParameterShape(internal_parameters_, base_node);
    }
  } catch (const std::exception &e) {
    if (strategy_ == GraphExecutionStrategy::kPipeline) {
      MsException::Instance().SetException();
    }
    std::string error_info = "Launch custom kernel exception: " + node->fullname_with_scope();
    SET_OPCONTEXT_FAIL_RET_WITH_ERROR_BY_STRATEGY(strategy_, (*ctx), error_info);
  }

  EraseInput(ctx);
  SendOutput(ctx);
}
}  // namespace runtime
}  // namespace mindspore
