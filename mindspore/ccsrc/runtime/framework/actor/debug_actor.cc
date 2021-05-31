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

#include "runtime/framework/actor/debug_actor.h"
#include "runtime/framework/actor/debug_aware_actor.h"
#include "mindrt/include/async/async.h"
#include "utils/log_adapter.h"

namespace mindspore {
namespace runtime {
void DebugActor::Debug(const AnfNodePtr &node, const DeviceContext *device_context, OpContext<DeviceTensor> *op_context,
                       const AID *from_aid) {
  MS_EXCEPTION_IF_NULL(node);
  MS_EXCEPTION_IF_NULL(device_context);
  MS_EXCEPTION_IF_NULL(op_context);
  MS_EXCEPTION_IF_NULL(from_aid);
  // todo debug.

  // Call back to the from actor to process after debug finished.
  Async(*from_aid, &DebugAwareActor::OnDebugFinish, op_context);
}

void DebugActor::DebugOnStepEnd(OpContext<DeviceTensor> *op_context, const AID *from_aid) {
  MS_EXCEPTION_IF_NULL(op_context);
  MS_EXCEPTION_IF_NULL(from_aid);
  // todo debug.

  // Call back to the from actor to process after debug finished.
  Async(*from_aid, &DebugAwareActor::OnDebugFinish, op_context);
}

}  // namespace runtime
}  // namespace mindspore
