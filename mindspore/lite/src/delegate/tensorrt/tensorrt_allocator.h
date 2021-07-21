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

#ifndef MINDSPORE_LITE_SRC_RUNTIME_DELEGATE_TENSORRT_TENSORRT_ALLOCATOR_H
#define MINDSPORE_LITE_SRC_RUNTIME_DELEGATE_TENSORRT_TENSORRT_ALLOCATOR_H
#include "src/delegate/tensorrt/tensorrt_allocator.h"
#include <map>
#include <string>
#include "include/api/types.h"
#include "include/ms_tensor.h"

namespace mindspore::lite {
struct CudaTensorParam {
  void *data;
  bool isValidMem;
};
class TensorRTAllocator {
 public:
  TensorRTAllocator() = default;

  ~TensorRTAllocator() = default;

  void *MallocDeviceMem(const mindspore::MSTensor &host_tensor, size_t size);

  void *GetDevicePtr(const std::string &tensor_name);

  int SyncMemInHostAndDevice(mindspore::MSTensor host_tensor, const std::string &device_tensor_name,
                             bool is_host2device, bool sync = true);

  int ClearDeviceMem();

 private:
  std::map<std::string, CudaTensorParam> cuda_tensor_map_;
};
}  // namespace mindspore::lite
#endif  // MINDSPORE_LITE_SRC_RUNTIME_DELEGATE_TENSORRT_TENSORRT_ALLOCATOR_H
