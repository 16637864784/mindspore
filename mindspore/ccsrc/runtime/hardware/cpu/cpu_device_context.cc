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

#include "runtime/hardware/cpu/cpu_device_context.h"
#include <string>
#include "runtime/device/cpu/cpu_device_address.h"
#include "runtime/device/cpu/cpu_memory_manager.h"
#include "backend/kernel_compiler/cpu/cpu_kernel_factory.h"
#include "runtime/device/cpu/kernel_select_cpu.h"
#include "utils/trace_base.h"
#include "backend/optimizer/common/optimizer.h"
#include "backend/optimizer/common/pass_manager.h"
#include "backend/optimizer/cpu/insert_cast_cpu.h"
#include "backend/optimizer/cpu/insert_format_transform_op.h"
#include "backend/optimizer/pass/replace_node_by_proxy.h"
#include "backend/optimizer/pass/erase_visit_attr.h"
#include "profiler/device/cpu/cpu_profiling.h"

namespace mindspore {
namespace device {
namespace cpu {
bool CPUDeviceContext::Initialize() {
  if (initialized_) {
    return true;
  }
  mem_manager_ = std::make_shared<CPUMemoryManager>();
  MS_EXCEPTION_IF_NULL(mem_manager_);
  initialized_ = true;
  return true;
}

bool CPUDeviceContext::AllocateMemory(DeviceAddress *const &address, size_t size) const {
  MS_EXCEPTION_IF_NULL(address);
  MS_EXCEPTION_IF_NULL(mem_manager_);
  auto device_ptr = mem_manager_->MallocMemFromMemPool(size);
  if (!device_ptr) {
    return false;
  }
  address->ptr_ = device_ptr;
  address->size_ = size;
  address->from_mem_pool_ = true;
  return true;
}

void CPUDeviceContext::FreeMemory(DeviceAddress *const &address) const {
  MS_EXCEPTION_IF_NULL(address);
  MS_EXCEPTION_IF_NULL(address->ptr_);
  MS_EXCEPTION_IF_NULL(mem_manager_);
  mem_manager_->FreeMemFromMemPool(address->ptr_);
  address->ptr_ = nullptr;
}

DeviceAddressPtr CPUDeviceContext::CreateDeviceAddress(void *device_ptr, size_t device_size, const string &format,
                                                       TypeId type_id) const {
  return std::make_shared<CPUDeviceAddress>(device_ptr, device_size, format, type_id);
}

void CPUDeviceContext::OptimizeGraph(const KernelGraphPtr &graph) const {
  // Update Graph Dynamic Shape Attr.
  UpdateGraphDynamicShapeAttr(NOT_NULL(graph));

  SetOperatorInfo(graph->execution_order());
  OptimizeGraphImpl(graph);

  // Remove reorder after PS feature finish adapting push/pull in auto_monad.
  auto execution_order = graph->execution_order();
  AnfAlgo::ReorderPosteriorExecList(NOT_NULL(&execution_order));
  graph->set_execution_order(execution_order);
}

void CPUDeviceContext::OptimizeSingleOpGraph(const KernelGraphPtr &graph) const {
  MS_EXCEPTION_IF_NULL(graph);
  SetOperatorInfo(graph->execution_order());
  OptimizeGraphImpl(graph);
}

void CPUDeviceContext::OptimizeGraphImpl(const KernelGraphPtr &graph) const {
  auto optimizer = std::make_shared<opt::GraphOptimizer>();
  auto pm = std::make_shared<opt::PassManager>();
  pm->AddPass(std::make_shared<opt::InsertCastCPU>("insert_cast_cpu"));
  pm->AddPass(std::make_shared<opt::InsertFormatTransformOpCPU>("insert_format_transform_op_cpu"));
  pm->AddPass(std::make_shared<opt::EraseVisitAttr>());
  optimizer->AddPassManager(pm);
  (void)optimizer->Optimize(graph);
  graph->SetExecOrderByDefault();
}

void CPUDeviceContext::UpdateGraphDynamicShapeAttr(const NotNull<KernelGraphPtr> &graph) const {
  for (const auto &cnode : graph->execution_order()) {
    if (AnfAlgo::IsNodeDynamicShape(cnode)) {
      AnfAlgo::SetNodeAttr(kAttrIsDynamicShape, MakeValue(true), cnode);
      MS_LOG(INFO) << "Set Dynamic Shape Attr to Node:" << cnode->fullname_with_scope();
    }
  }
  graph->UpdateGraphDynamicAttr();
}

void CPUDeviceContext::SetOperatorInfo(const std::vector<CNodePtr> &nodes) const {
  for (const auto &node : nodes) {
    SetKernelInfo(node);
  }
}

void CPUDeviceContext::CreateKernel(const std::vector<CNodePtr> &nodes) const {
  for (const auto &node : nodes) {
    MS_EXCEPTION_IF_NULL(node);
    std::string kernel_name = AnfAlgo::GetCNodeName(node);
    std::shared_ptr<kernel::CPUKernel> cpu_kernel = kernel::CPUKernelFactory::GetInstance().Create(kernel_name, node);
    if (!cpu_kernel) {
      MS_LOG(EXCEPTION) << "Build cpu operator[" << node->fullname_with_scope() << "] failed";
    }

    cpu_kernel->Init(node);
    AnfAlgo::SetKernelMod(cpu_kernel, node.get());
  }
}

bool CPUDeviceContext::LaunchKernel(const CNodePtr &kernel, const std::vector<AddressPtr> &inputs,
                                    const std::vector<AddressPtr> &workspace,
                                    const std::vector<AddressPtr> &outputs) const {
  MS_EXCEPTION_IF_NULL(kernel);

  auto profiler_inst = profiler::cpu::CPUProfiler::GetInstance();
  MS_EXCEPTION_IF_NULL(profiler_inst);
  if (profiler_inst->GetEnableFlag()) {
    return LaunchKernelWithProfiling(kernel, inputs, workspace, outputs);
  }

  auto kernel_mod = AnfAlgo::GetKernelMod(kernel);
  MS_EXCEPTION_IF_NULL(kernel_mod);
  return DoLaunchKernel(kernel_mod, inputs, workspace, outputs);
}

bool CPUDeviceContext::LaunchKernelWithProfiling(const CNodePtr &kernel, const std::vector<AddressPtr> &inputs,
                                                 const std::vector<AddressPtr> &workspace,
                                                 const std::vector<AddressPtr> &outputs) const {
  MS_EXCEPTION_IF_NULL(kernel);
  std::lock_guard<std::mutex> locker(launch_mutex_);

  auto profiler_inst = profiler::cpu::CPUProfiler::GetInstance();
  MS_EXCEPTION_IF_NULL(profiler_inst);

  auto kernel_mod = AnfAlgo::GetKernelMod(kernel);
  MS_EXCEPTION_IF_NULL(kernel_mod);

  uint32_t pid = getpid();
  profiler_inst->OpDataProducerBegin(kernel->fullname_with_scope(), pid);
  bool ret = DoLaunchKernel(kernel_mod, inputs, workspace, outputs);
  profiler_inst->OpDataProducerEnd();

  return ret;
}

bool CPUDeviceContext::DoLaunchKernel(KernelMod *kernel_mod, const std::vector<AddressPtr> &inputs,
                                      const std::vector<AddressPtr> &workspace,
                                      const std::vector<AddressPtr> &outputs) const {
  MS_EXCEPTION_IF_NULL(kernel_mod);
  return kernel_mod->Launch(inputs, workspace, outputs, nullptr);
}

MS_REGISTER_DEVICE(kCPUDevice, CPUDeviceContext);
}  // namespace cpu
}  // namespace device
}  // namespace mindspore
