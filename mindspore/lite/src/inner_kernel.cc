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

#include "src/inner_kernel.h"
#include <algorithm>
#include <set>
#include "src/tensor.h"
#include "src/common/utils.h"
#include "src/runtime/infer_manager.h"

namespace mindspore::kernel {
using mindspore::lite::RET_ERROR;
using mindspore::lite::RET_OK;

void *InnerKernel::workspace_ = nullptr;

void InnerKernel::AllocWorkspace(size_t size) {
  if (size == 0) {
    return;
  }
  workspace_ = malloc(size);
  if (workspace_ == nullptr) {
    MS_LOG(ERROR) << "fail to alloc " << size;
  }
}

void InnerKernel::FreeWorkspace() {
  free(workspace_);
  workspace_ = nullptr;
}

int InnerKernel::PreProcess() {
  if (!InferShapeDone()) {
    auto ret = lite::KernelInferShape(in_tensors_, out_tensors_, op_parameter_);
    if (ret != 0) {
      MS_LOG(ERROR) << "InferShape fail!";
      return ret;
    }
    ret = ReSize();
    if (ret != 0) {
      MS_LOG(ERROR) << "ReSize fail!ret: " << ret;
      return ret;
    }
  }

  for (auto *output : this->out_tensors()) {
    MS_ASSERT(output != nullptr);
    if (registry_data_type_ == kNumberTypeFloat16 && output->data_type() == kNumberTypeFloat32) {
      output->set_data_type(kNumberTypeFloat16);
    }

    if (output->ElementsNum() >= MAX_MALLOC_SIZE / static_cast<int>(sizeof(int64_t))) {
      MS_LOG(ERROR) << "The size of output tensor is too big";
      return RET_ERROR;
    }
    auto ret = output->MallocData();
    if (ret != RET_OK) {
      MS_LOG(ERROR) << "MallocData failed";
      return ret;
    }
  }
  return RET_OK;
}
}  // namespace mindspore::kernel
