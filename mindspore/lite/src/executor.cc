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

#include "src/executor.h"
#include <queue>
#include "include/errorcode.h"
#include "src/common/tensor_util.h"

namespace mindspore::lite {
int Executor::Run(const std::vector<Tensor *> &in_tensors, const std::vector<Tensor *> &out_tensors,
                  const std::vector<kernel::LiteKernel *> &kernels, mindspore::Allocator *allocator,
                  const KernelCallBack &before, const KernelCallBack &after) {
  CHECK_NULL_RETURN(allocator);

  // init the max spin count.
  CHECK_NULL_RETURN(ctx_);
  auto thread_pool = ctx_->thread_pool();
  CHECK_NULL_RETURN(thread_pool);
  thread_pool->SetMaxSpinCount(kDefaultSpinCount);

  // clear ref_count
  for (auto *kernel : kernels) {
    for (auto *tensor : kernel->in_tensors()) {
      tensor->set_ref_count(0);
    }
  }
  std::queue<kernel::LiteKernel *> kernel_queue;
  for (auto kernel : kernels) {
    if (kernel->IsReady(kernel->in_tensors())) {
      kernel_queue.push(kernel);
    }
  }
  while (!kernel_queue.empty()) {
    auto cur_kernel = kernel_queue.front();
    kernel_queue.pop();
    MS_ASSERT(cur_kernel != nullptr);
    int ret = cur_kernel->Execute(before, after);
    if (ret != RET_OK) {
      MS_LOG(ERROR) << "run kernel failed, name: " << cur_kernel->name();
      return ret;
    }
    for (auto &out_kernel : cur_kernel->out_kernels()) {
      if (out_kernel->IsReady(out_kernel->in_tensors())) {
        kernel_queue.push(out_kernel);
      }
    }
  }

  // reset the max spin count.
  thread_pool->SetMaxSpinCount(kMinSpinCount);
  return RET_OK;
}
}  // namespace mindspore::lite
