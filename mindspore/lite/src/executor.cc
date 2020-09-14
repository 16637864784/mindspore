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

#include "mindspore/lite/src/executor.h"
#include "nnacl/pack.h"
#include "include/errorcode.h"

namespace mindspore::lite {
int Executor::Run(std::vector<Tensor *> &in_tensors, std::vector<Tensor *> &out_tensors,
                  std::vector<kernel::LiteKernel *> &kernels, Allocator *allocator,
                  const session::KernelCallBack &before, const session::KernelCallBack &after) {
  MS_ASSERT(nullptr != allocator);
  for (auto &inTensor : in_tensors) {
    if (inTensor == nullptr) {
      MS_LOG(ERROR) << "Graph input tensor is nullptr";
      return RET_ERROR;
    }
    if (inTensor->MutableData() == nullptr) {
      MS_LOG(ERROR) << "Graph input tensor data is nullptr";
      return RET_ERROR;
    }
    if (inTensor->GetFormat() != schema::Format::Format_NHWC) {
      MS_LOG(ERROR) << "Model input tensor should be NHWC";
      return RET_ERROR;
    }
  }
  kernel::LiteKernelUtil::InitTensorRefCount(kernels);
  for (auto out_tensor : out_tensors) {  // increase RefCount of output tensors, such that Run will not free them
    out_tensor->SetRefCount(out_tensor->RefCount() + 1);
  }

  for (auto *kernel : kernels) {
    MS_ASSERT(nullptr != kernel);

    if (before != nullptr) {
      if (!before(TensorVectorCast(kernel->in_tensors()), TensorVectorCast(kernel->out_tensors()),
                  {kernel->name(), kernel->type_str()})) {
        MS_LOG(ERROR) << "run kernel before_callback failed, name: " << kernel->name();
      }
    }
    auto ret = kernel->Run();
    if (0 != ret) {
      MS_LOG(ERROR) << "run kernel failed, name: " << kernel->name();
      return ret;
    }
    if (after != nullptr) {
      if (!after(TensorVectorCast(kernel->in_tensors()), TensorVectorCast(kernel->out_tensors()),
                 {kernel->name(), kernel->type_str()})) {
        MS_LOG(ERROR) << "run kernel after_callback failed, name: " << kernel->name();
      }
    }
    for (auto input_kernel : kernel->in_kernels()) {
      MS_ASSERT(input_kernel != nullptr);
      if (input_kernel->is_model_output()) {
        continue;
      }
      ret = input_kernel->DecOutTensorRefCount();
      if (0 != ret) {
        MS_LOG(WARNING) << "DecOutTensorRefCount for kernel" << kernel->name() << " failed";
      }
    }
  }
  return RET_OK;
}

int Executor::TransformTensorLayout(Tensor *tensor, schema::Format dst_format, Allocator *allocator) {
  MS_ASSERT(nullptr != tensor);
  MS_ASSERT(nullptr != allocator);
  MS_ASSERT(4 == tensor->shape().size());
  auto data_type = tensor->data_type();
  switch (data_type) {
    case kNumberTypeInt8:
      return TransformTensorLayoutUint8(tensor, dst_format, allocator);
    case kNumberTypeFloat32:
      return TransformTensorLayoutFp32(tensor, dst_format, allocator);
    default:
      return RET_ERROR;
  }
  return RET_OK;
}

int Executor::TransformTensorLayoutFp32(Tensor *tensor, schema::Format dst_format, Allocator *allocator) {
  MS_ASSERT(nullptr != tensor);
  MS_ASSERT(nullptr != allocator);
  MS_ASSERT(4 == tensor->shape().size());
  auto src_format = tensor->GetFormat();
  if (src_format == schema::Format::Format_NC4HW4 && dst_format == schema::Format::Format_NHWC) {
    auto *src_data = tensor->MutableData();
    if (src_data == nullptr) {
      MS_LOG(ERROR) << "MutableData return nullptr";
      return RET_ERROR;
    }
    auto *dst_data = allocator->Malloc(tensor->Size());
    if (dst_data == nullptr) {
      MS_LOG(ERROR) << "Malloc data failed";
      return RET_ERROR;
    }
    PackNC4HW4ToNHWCFp32(src_data, dst_data, tensor->Batch(), tensor->Height() * tensor->Width(), tensor->Channel());
    tensor->SetData(dst_data);
    tensor->SetFormat(dst_format);
    allocator->Free(src_data);
    return RET_OK;
  } else {
    MS_LOG(ERROR) << "Unsupported layout transform: " << EnumNameFormat(tensor->GetFormat()) << " to "
                  << EnumNameFormat(dst_format) << " in float32";
    return RET_ERROR;
  }
}

int Executor::TransformTensorLayoutUint8(Tensor *tensor, schema::Format dst_format, Allocator *allocator) {
  MS_ASSERT(nullptr != tensor);
  MS_ASSERT(nullptr != allocator);
  MS_ASSERT(4 == tensor->shape().size());
  MS_LOG(ERROR) << "Unsupported layout transform: " << EnumNameFormat(tensor->GetFormat()) << " to "
                << EnumNameFormat(dst_format) << " in uint8";
  return RET_ERROR;
}
}  // namespace mindspore::lite
