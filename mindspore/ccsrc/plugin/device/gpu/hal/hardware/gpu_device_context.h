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

#ifndef MINDSPORE_CCSRC_RUNTIME_HARDWARE_GPU_GPU_DEVICE_CONTEXT_H_
#define MINDSPORE_CCSRC_RUNTIME_HARDWARE_GPU_GPU_DEVICE_CONTEXT_H_

#include <vector>
#include <memory>
#include <string>
#include "runtime/hardware/device_context.h"
#include "runtime/hardware/device_context_manager.h"
#include "runtime/device/memory_manager.h"

namespace mindspore {
namespace device {
namespace gpu {
class GPUDeviceContext : public DeviceContext {
 public:
  explicit GPUDeviceContext(const DeviceContextKey &device_context_key)
      : DeviceContext(device_context_key), mem_manager_(nullptr), initialized_(false) {}
  ~GPUDeviceContext() override = default;

  // Set device id and initialize device resource, such as stream, cudnn and cublas handle.
  void Initialize() override;

  // Release device memory, stream, cudnn and cublas handle, etc.
  void Destroy() override;

  bool BindDeviceToCurrentThread() const override;

  // Relevant function to allocate and free device memory of raw ptr.
  void *AllocateMemory(size_t size) const override;
  void FreeMemory(void *ptr) const override;
  std::vector<void *> AllocateContinuousMemory(const std::vector<size_t> &size_list) const override;

  DeviceAddressPtr CreateDeviceAddress(void *const device_ptr, size_t device_size, const string &format, TypeId type_id,
                                       const ShapeVector &shape = ShapeVector()) const override;

  // Optimize the kernel graph for graph mode.
  void OptimizeGraph(const FuncGraphPtr &graph) const override;

  void CreateKernel(const std::vector<CNodePtr> &nodes) const override;

  bool LaunchKernel(const CNodePtr &kernel, const std::vector<AddressPtr> &inputs,
                    const std::vector<AddressPtr> &workspace, const std::vector<AddressPtr> &outputs) const override;

  bool SyncStream(size_t stream_id = 0) const override;

  uint32_t GetRankID() const override;

  // Create bucket for every allreduce operator. Bucket is used in PyNative distributed training mode, one bucket
  // handles all resource to launch and sync allreduce operator.
  std::shared_ptr<Bucket> CreateBucket(uint32_t bucket_id, uint32_t bucket_size) const override;

  bool LoadCollectiveCommLib() override;

  void PreprocessBeforeRun(const FuncGraphPtr &graph) const override;

 private:
  DISABLE_COPY_AND_ASSIGN(GPUDeviceContext);
  bool InitDevice();

  // Select the matching backend kernels according to the data type and format of input and output for all
  // execution operators, and set final device data type and format information for backend kernels, device
  // data type and format which replace original data type and format will use for executing kernels.
  void SetOperatorInfo(const KernelGraphPtr &graph) const;

  // General graph optimezer ignore device data type and format.
  void OptimizeGraphWithoutDeviceInfo(const KernelGraphPtr &graph) const;
  // Optimize the kernel graph according to device type, such format transform.
  void OptimizeGraphWithDeviceInfo(const KernelGraphPtr &graph) const;

  // Operator fusion optimization.
  void FuseOperators(const KernelGraphPtr &graph) const;

  // Update kernel ref info before create kernel
  void UpdateKernelRefInfo(const KernelGraphPtr &graph) const;

#ifndef ENABLE_SECURITY
  // Launch a kernel and record the elapsed time end to end.
  bool LaunchKernelWithProfiling(const CNodePtr &kernel, const std::vector<AddressPtr> &inputs,
                                 const std::vector<AddressPtr> &workspace, const std::vector<AddressPtr> &outputs,
                                 void *stream) const;
#endif
  // Launch a kernel by 'KernelMod' of the kernel.
  bool DoLaunchKernel(const CNodePtr &kernel, const std::vector<AddressPtr> &inputs,
                      const std::vector<AddressPtr> &workspace, const std::vector<AddressPtr> &outputs,
                      void *stream) const;

  // Get the used to launch kernel, if there is a stream saved in attrs of kernel, use this stream, otherwise use
  // default stream.
  void *GetLaunchKernelStream(const CNodePtr &kernel) const;

  // Really create a cuda stream.
  bool CreateStream(void **stream) const override;

  // Really destroy a cuda stream.
  bool DestroyStream(void *stream) const override;

  std::shared_ptr<MemoryManager> mem_manager_;
  std::vector<void *> streams_;
  bool initialized_;
};
}  // namespace gpu
}  // namespace device
}  // namespace mindspore
#endif  // MINDSPORE_CCSRC_RUNTIME_HARDWARE_GPU_GPU_DEVICE_CONTEXT_H_
