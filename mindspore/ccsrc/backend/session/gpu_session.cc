/**
 * Copyright 2019-2020 Huawei Technologies Co., Ltd
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
#include "backend/session/gpu_session.h"

#include "runtime/device/gpu/kernel_info_setter.h"
#include "runtime/device/gpu/gpu_kernel_build.h"
#include "runtime/device/gpu/gpu_kernel_runtime.h"
#include "runtime/device/gpu/gpu_stream_assign.h"
#include "backend/optimizer/common/optimizer.h"
#include "backend/optimizer/common/pass_manager.h"
#include "backend/optimizer/common/helper.h"
#include "backend/optimizer/pass/communication_op_fusion.h"
#include "backend/optimizer/pass/getitem_tuple.h"
#include "backend/optimizer/gpu/adam_weight_decay_fusion.h"
#include "backend/optimizer/gpu/adam_fusion.h"
#include "backend/optimizer/gpu/apply_momentum_weight_scale_fusion.h"
#include "backend/optimizer/gpu/apply_momentum_scale_fusion.h"
#include "backend/optimizer/gpu/replace_bn_cast_fusion.h"
#include "backend/optimizer/gpu/replace_bn_grad_cast_fusion.h"
#include "backend/optimizer/gpu/batch_norm_relu_fusion.h"
#include "backend/optimizer/gpu/batch_norm_relu_grad_fusion.h"
#include "backend/optimizer/gpu/batch_norm_add_relu_fusion.h"
#include "backend/optimizer/gpu/batch_norm_add_relu_grad_fusion.h"
#include "backend/optimizer/gpu/replace_momentum_cast_fusion.h"
#include "backend/optimizer/gpu/replace_addn_fusion.h"
#include "backend/optimizer/gpu/insert_format_transform_op.h"
#include "backend/optimizer/gpu/remove_format_transform_pair.h"
#include "backend/optimizer/gpu/remove_redundant_format_transform.h"
#include "backend/optimizer/graph_kernel/graph_kernel_splitter.h"
#include "backend/optimizer/graph_kernel/graph_kernel_expander.h"
#include "backend/optimizer/graph_kernel/basic_ops_fusion.h"
#include "backend/optimizer/graph_kernel/composite_ops_fusion.h"
#include "runtime/device/kernel_runtime_manager.h"
#include "utils/ms_utils.h"
#include "utils/config_manager.h"
#include "common/trans.h"
#include "utils/ms_context.h"
#include "debug/data_dump/e2e_dump_util.h"
#include "debug/tensor_load.h"
#include "debug/dump_proto.h"

namespace mindspore {
namespace session {
namespace gpu {
using AnfAlgo = mindspore::session::AnfRuntimeAlgorithm;

void GPUSession::SelectKernel(const std::shared_ptr<KernelGraph> &kernel_graph) const {
  MS_EXCEPTION_IF_NULL(kernel_graph);
  device::gpu::FormatTransformChecker::GetInstance().CheckSupportFormatTransform(kernel_graph);
  for (const auto &kernel_node : kernel_graph->execution_order()) {
    MS_EXCEPTION_IF_NULL(kernel_node);
    device::gpu::SetKernelInfo(kernel_node);
  }
}

void GPUSession::StartKernelRT() const {
  auto runtime_instance = device::KernelRuntimeManager::Instance().GetSingleKernelRuntime(kGPUDevice, device_id_);
  MS_EXCEPTION_IF_NULL(runtime_instance);
  if (!runtime_instance->Init()) {
    MS_LOG(EXCEPTION) << "GPU start kernel runtime failed";
  }
}

void GPUSession::Optimize(const std::shared_ptr<KernelGraph> &kernel_graph) {
  MS_EXCEPTION_IF_NULL(kernel_graph);
  auto context_ptr = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(context_ptr);
  auto optimizer = std::make_shared<opt::GraphOptimizer>();
  auto pm = std::make_shared<opt::PassManager>();
  pm->AddPass(std::make_shared<opt::AdamWeightDecayFusion>());
  pm->AddPass(std::make_shared<opt::AdamFusion>());
  pm->AddPass(std::make_shared<opt::ReplaceBNCastFusion>());
  pm->AddPass(std::make_shared<opt::ReplaceBNGradCastFusion>());
  pm->AddPass(std::make_shared<opt::ReplaceMomentumCastFusion>());
  pm->AddPass(std::make_shared<opt::ReplaceAddNFusion>());
  optimizer->AddPassManager(pm);
  (void)optimizer->Optimize(kernel_graph);
  kernel_graph->SetExecOrderByDefault();
}

void GPUSession::HardwareOptimize(const std::shared_ptr<KernelGraph> &kernel_graph) {
  auto optimizer = std::make_shared<opt::GraphOptimizer>();
  auto pm = std::make_shared<opt::PassManager>();
  pm->AddPass(std::make_shared<opt::BatchNormReluFusion>());
  pm->AddPass(std::make_shared<opt::BatchNormReluGradFusion>());
  pm->AddPass(std::make_shared<opt::BatchNormAddReluFusion>());
  pm->AddPass(std::make_shared<opt::InsertFormatTransformOp>());
  pm->AddPass(std::make_shared<opt::RemoveFormatTransformPair>());
  pm->AddPass(std::make_shared<opt::RemoveRedundantFormatTransform>());
  pm->AddPass(std::make_shared<opt::AllReduceFusion>());
  pm->AddPass(std::make_shared<opt::GetitemTuple>());
  optimizer->AddPassManager(pm);
  (void)optimizer->Optimize(kernel_graph);
  kernel_graph->SetExecOrderByDefault();
}

void GPUSession::GraphKernelOptimize(const std::shared_ptr<KernelGraph> &kernel_graph) {
  auto context_ptr = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(context_ptr);
  if (!(context_ptr->get_param<bool>(MS_CTX_ENABLE_GRAPH_KERNEL))) {
    return;
  }
  auto optimizer = std::make_shared<opt::GraphOptimizer>();
  auto pm = std::make_shared<opt::PassManager>("graph_kernel_pm");
  pm->AddPass(std::make_shared<opt::GraphKernelExpander>());
  pm->AddPass(std::make_shared<opt::BasicOpsFusion>());
  pm->AddPass(std::make_shared<opt::CompositeOpsFusion>());
  pm->AddPass(std::make_shared<opt::GraphKernelSplitter>());
  optimizer->AddPassManager(pm);
  (void)optimizer->Optimize(kernel_graph);
  kernel_graph->SetExecOrderByDefault();
}

void GPUSession::AssignStream(const std::shared_ptr<KernelGraph> &kernel_graph) {
  MS_EXCEPTION_IF_NULL(kernel_graph);
  device::gpu::AssignGpuStream(kernel_graph);
}

void GPUSession::BuildKernel(const std::shared_ptr<KernelGraph> &kernel_graph) const {
  device::gpu::GpuBuild(kernel_graph);
}

void GPUSession::AllocateMemory(KernelGraph *kernel_graph) const {
  MS_EXCEPTION_IF_NULL(kernel_graph);
  auto runtime_instance = device::KernelRuntimeManager::Instance().GetSingleKernelRuntime(kGPUDevice, device_id_);
  MS_EXCEPTION_IF_NULL(runtime_instance);
  runtime_instance->AssignMemory(kernel_graph);
}

void GPUSession::RunOpAllocateMemory(const ValuePtr &pre_output_value,
                                     const std::vector<tensor::TensorPtr> &input_tensors,
                                     KernelGraph *kernel_graph) const {
  MS_EXCEPTION_IF_NULL(kernel_graph);
  auto runtime_instance = device::KernelRuntimeManager::Instance().GetSingleKernelRuntime(kGPUDevice, device_id_);
  MS_EXCEPTION_IF_NULL(runtime_instance);
  runtime_instance->RunOpAssignMemory(pre_output_value, input_tensors, kernel_graph);
}

void GPUSession::RunOpClearMemory(KernelGraph *kernel_graph) const {
  MS_EXCEPTION_IF_NULL(kernel_graph);
  auto runtime_instance = device::KernelRuntimeManager::Instance().GetSingleKernelRuntime(kGPUDevice, device_id_);
  MS_EXCEPTION_IF_NULL(runtime_instance);
  runtime_instance->RunOpClearMemory(kernel_graph);
}

void GPUSession::LoadInputData(const std::shared_ptr<KernelGraph> &kernel_graph,
                               const std::vector<tensor::TensorPtr> &inputs_const) const {
  std::vector<tensor::TensorPtr> inputs(inputs_const);
  MS_EXCEPTION_IF_NULL(kernel_graph);
  std::vector<AnfNodePtr> input_nodes;
  for (const auto &input_node : kernel_graph->inputs()) {
    auto params = AnfAlgo::GetAllOutput(input_node);
    std::copy(params.begin(), params.end(), std::back_inserter(input_nodes));
  }
  auto ms_context = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(ms_context);
  if (inputs.size() != input_nodes.size()) {
    MS_LOG(EXCEPTION) << "Tensor input:" << inputs.size() << " is not equal graph inputs:" << input_nodes.size();
  }
  for (size_t i = 0; i < inputs.size(); ++i) {
    auto tensor = inputs[i];
    MS_EXCEPTION_IF_NULL(tensor);
    auto input_node = input_nodes[i];
    MS_EXCEPTION_IF_NULL(input_node);
    if (input_node->isa<Parameter>() && AnfAlgo::OutputAddrExist(input_node, 0)) {
      auto pk_node = input_node->cast<ParameterPtr>();
      auto device_address = AnfAlgo::GetMutableOutputAddr(pk_node, 0);
      auto tensor_address = std::dynamic_pointer_cast<device::DeviceAddress>(tensor->device_address());
      bool need_sync = false;
      if (ms_context->get_param<bool>(MS_CTX_ENABLE_PYNATIVE_INFER)) {
        if (tensor_address == nullptr || tensor_address != device_address) {
          need_sync = true;
        }
      } else if (tensor->NeedSyncHostToDevice() || tensor_address == nullptr) {
        need_sync = true;
      } else if (tensor_address != device_address) {
        if (tensor_address->DeviceType() == device_address->DeviceType()) {
          AnfAlgo::SetOutputAddr(tensor_address, 0, pk_node.get());
        } else {
          need_sync = true;
        }
      }
      if (need_sync) {
        if (AnfAlgo::IsParameterWeight(input_node->cast<ParameterPtr>())) {
          tensor->set_device_address(device_address);
        }
        MS_EXCEPTION_IF_NULL(device_address);
        if (!device_address->SyncHostToDevice(trans::GetRuntimePaddingShape(pk_node, 0),
                                              LongToSize(tensor->data().nbytes()), tensor->data_type(),
                                              tensor->data_c())) {
          MS_LOG(EXCEPTION) << "SyncHostToDevice failed.";
        }
      }
    }
    tensor->set_sync_status(kNoNeedSync);
  }
}

void GPUSession::Execute(const std::shared_ptr<KernelGraph> &kernel_graph) const {
  auto runtime_instance = device::KernelRuntimeManager::Instance().GetSingleKernelRuntime(kGPUDevice, device_id_);
  MS_EXCEPTION_IF_NULL(runtime_instance);
#ifdef ENABLE_DEBUGGER
  if (!runtime_instance->Run(kernel_graph.get(), false, debugger_.get())) {
#else
  if (!runtime_instance->Run(kernel_graph.get(), false)) {
#endif
    MS_LOG(EXCEPTION) << "GPU execute graph failed!";
  }
}

GraphId GPUSession::CompileGraph(const AnfNodePtrList &lst, const AnfNodePtrList &outputs) {
  // Construct graph, if successfully, graph_sum_ + 1
  auto graph_id = graph_sum_;
  auto graph = ConstructKernelGraph(lst, outputs);
  MS_EXCEPTION_IF_NULL(graph);
  // Prepare ms context info for dump .pb graph
  auto context_ptr = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(context_ptr);
  bool save_graphs = context_ptr->get_param<bool>(MS_CTX_SAVE_GRAPHS_FLAG);
  // Dump .pb graph before graph optimization
  if (save_graphs) {
    DumpIRProto(graph, "before_opt_" + std::to_string(graph_id));
  }
  // Graph optimization irrelevant to device data format
  Optimize(graph);
  // Select kernel build info
  SelectKernel(graph);
  // Graph optimization relevant to device data format
  HardwareOptimize(graph);
  // Graph kernel fusion optimization
  GraphKernelOptimize(graph);
  // Dump .pb graph after graph optimization
  if (save_graphs) {
    DumpIRProto(graph, "after_opt_" + std::to_string(graph_id));
  }

#if (ENABLE_CPU && (ENABLE_D || ENABLE_GPU))
  // Assign parameter keys.
  AssignParamKey(graph);
#endif
  // Start gpu kernel runtime
  StartKernelRT();
  // Assign CUDA streams
  AssignStream(graph);
  // Hide NopOp from execution graph
  opt::HideNopNode(graph.get());
  // Build kernel if node is cnode
  BuildKernel(graph);
  // Set graph execution order before memory alloc, ensure that memory alloc is according to the reorder graph
  auto execution_order = graph->execution_order();
  Reorder(&execution_order);
  graph->set_execution_order(execution_order);
  // Get summary nodes.
  SetSummaryNodes(graph.get());
  // Remove NopOp from execution graph
  opt::RemoveNopNode(graph.get());
  // Set graph manager.
  MS_EXCEPTION_IF_NULL(context_);
  FuncGraphManagerPtr manager = MakeManager({graph});
  context_->AddManager(manager);
  if (manager) {
    manager->AddFuncGraph(graph);
    graph->set_manager(manager);
  }
  // Alloc memory, including static memory and dynamic memory
  AllocateMemory(graph.get());
  return graph_id;
}

void GPUSession::RunGraph(const GraphId &graph_id, const std::vector<tensor::TensorPtr> &inputs, VectorRef *outputs) {
  auto &kernel_graph = graphs_[graph_id];
  PreIterationDbg(kernel_graph);
  // Load input data from user input
  LoadInputData(kernel_graph, inputs);
#if (ENABLE_CPU && (ENABLE_D || ENABLE_GPU))
  // Initialize parameter server
  InitPSParamAndOptim(kernel_graph, inputs);
#endif
  MS_EXCEPTION_IF_NULL(kernel_graph);
  Execute(kernel_graph);
  PostLoadTensor(kernel_graph);
  // Summary
  auto context_ptr = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(context_ptr);
  if (context_ptr->get_param<bool>(MS_CTX_ENABLE_GPU_SUMMARY)) {
    Summary(kernel_graph.get());
  }
  PostIterationDbg(kernel_graph);
}

void GPUSession::BuildOp(const OpRunInfo &op_run_info, const GraphInfo &graph_info,
                         const std::vector<tensor::TensorPtr> &input_tensors, const std::vector<int> &tensors_mask) {
  // Check if the graph cache exists.
  if (run_op_graphs_.find(graph_info) != run_op_graphs_.end()) {
    return;
  }
  // Prepare the graph
  auto kernel_graph = ConstructSingleOpGraph(op_run_info, input_tensors, tensors_mask);
  MS_EXCEPTION_IF_NULL(kernel_graph);
  SelectKernel(kernel_graph);
  StartKernelRT();
  // Hide NopOp from execution graph
  opt::HideNopNode(kernel_graph.get());
  BuildKernel(kernel_graph);
  run_op_graphs_[graph_info] = kernel_graph;
}

void GPUSession::RunOp(const OpRunInfo &op_run_info, const GraphInfo &graph_info,
                       const std::vector<tensor::TensorPtr> &input_tensors, VectorRef *outputs) {
  auto kernel_graph = run_op_graphs_[graph_info];
  MS_EXCEPTION_IF_NULL(kernel_graph);
  // Remove NopOp from execution graph
  opt::RemoveNopNode(kernel_graph.get());
  RunOpAllocateMemory(op_run_info.value, input_tensors, kernel_graph.get());
  // Execute the computation
  LoadInputData(kernel_graph, input_tensors);
  Execute(kernel_graph);
  // Fetch outputs
  UpdateOutputs(kernel_graph, outputs, input_tensors);
  RunOpClearMemory(kernel_graph.get());
}

void GPUSession::Dump(const std::shared_ptr<KernelGraph> &kernel_graph) const {
  if (debugger_->DebuggerBackendEnabled()) {
    MS_EXCEPTION_IF_NULL(kernel_graph);
    E2eDumpUtil::DumpData(kernel_graph.get(), debugger_.get());
  } else {
    DumpJsonParser::GetInstance().UpdateDumpIter();
  }
}

bool GPUSession::DumpDataEnabledIteration() const {
  auto runtime_instance = device::KernelRuntimeManager::Instance().GetSingleKernelRuntime(kGPUDevice, device_id_);
  MS_EXCEPTION_IF_NULL(runtime_instance);
  return runtime_instance->DumpDataEnabledIteration();
}

void GPUSession::PreIterationDbg(const std::shared_ptr<KernelGraph> &kernel_graph) const {
  if (debugger_) {
    debugger_->PreExecute(kernel_graph);
  }
  PreLoadTensor(kernel_graph);
}

void GPUSession::PostIterationDbg(const std::shared_ptr<KernelGraph> &kernel_graph) const {
  bool dump_enabled = DumpDataEnabledIteration();
  // debug used for dump
  if (debugger_ && dump_enabled) {
    Dump(kernel_graph);
  } else {
    DumpJsonParser::GetInstance().UpdateDumpIter();
  }
  if (debugger_) {
    debugger_->PostExecute();
  }
}

void GPUSession::PreLoadTensor(const std::shared_ptr<KernelGraph> &kernel_graph) const {
  // check the dump_enabled and dataset_sink_mode
  bool dump_enabled = DumpDataEnabledIteration();
  auto context_ptr = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(context_ptr);
  if (dump_enabled && ConfigManager::GetInstance().dataset_mode() == DS_SINK_MODE) {
    MS_EXCEPTION(NotSupportError) << "Don't support set dataset_sink_mode to True when using e2e_dump";
  }

  if (!(debugger_ && (debugger_->debugger_enabled() || dump_enabled))) {
    return;
  }
  MS_EXCEPTION_IF_NULL(kernel_graph);
  auto runtime_instance = device::KernelRuntimeManager::Instance().GetSingleKernelRuntime(kGPUDevice, device_id_);
  MS_EXCEPTION_IF_NULL(runtime_instance);
  DebugServices *debug_services = debugger_->debug_services();
  TensorLoader *tensor_loader = debug_services->tensor_loader();
  tensor_loader->EmptyTensor();
  uint32_t iter_num = tensor_loader->GetIterNum();
  tensor_loader->set_iter_num(++iter_num);
}

void GPUSession::PostLoadTensor(const std::shared_ptr<KernelGraph> &kernel_graph) const {
  bool dump_enabled = DumpDataEnabledIteration();
  if (!(debugger_ && (debugger_->debugger_enabled() || dump_enabled))) {
    return;
  }
  MS_EXCEPTION_IF_NULL(kernel_graph);
  auto runtime_instance = device::KernelRuntimeManager::Instance().GetSingleKernelRuntime(kGPUDevice, device_id_);
  MS_EXCEPTION_IF_NULL(runtime_instance);
  DebugServices *debug_services = debugger_->debug_services();
  TensorLoader *tensor_loader = debug_services->tensor_loader();
  tensor_loader->EmptyPrevTensor();
}
}  // namespace gpu
}  // namespace session
}  // namespace mindspore
