/**
 * Copyright 2020-2021 Huawei Technologies Co., Ltd
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

#ifndef MINDSPORE_LITE_SRC_SCHEDULER_H_
#define MINDSPORE_LITE_SRC_SCHEDULER_H_

#include <utility>
#include <vector>
#include <memory>
#include <map>
#include <deque>
#include <unordered_map>
#include <set>
#include "src/sub_graph_kernel.h"
#include "src/inner_context.h"
#include "include/model.h"
#include "src/scheduler_cb.h"

#include "include/delegate.h"

namespace mindspore::lite {
class Scheduler {
 public:
  Scheduler(const InnerContext *ctx, Model *src_model, std::vector<Tensor *> *src_tensors,
            const std::vector<Tensor *> &input_tensors, const std::vector<Tensor *> &output_tensors,
            bool is_train_session, std::shared_ptr<Delegate> delegate = nullptr)
      : context_(ctx),
        src_model_(src_model),
        src_tensors_(src_tensors),
        inputs_(input_tensors),
        outputs_(output_tensors),
        is_train_session_(is_train_session),
        delegate_(delegate) {}
  ~Scheduler() = default;
  int Schedule(std::vector<kernel::LiteKernel *> *dst_kernels);
  void SetupSchedulerCb(std::unique_ptr<SchedulerCb> cb) { sched_cb_ = std::move(cb); }

 private:
  void FindNodeInoutTensors(const Model::Node &node, std::vector<Tensor *> *inputs, std::vector<Tensor *> *outputs);
  Model::Node *NodeInputIsPartial(const Model::Node *node);
  int InferPartialShape(const Model::Node *node);
  Model::Node *NodeInputIsSwitch(const Model::Node *node);
  int InferSwitchShape(const Model::Node *node);
  int InferCallShape(const Model::Node *node);
  int InferNodeShape(const Model::Node *node);
  int InferSubGraphShape(size_t subgraph_index);
  // schedule a node to kernel according to context and kernels registered
  kernel::LiteKernel *FindBackendKernel(const std::vector<Tensor *> &in_tensors,
                                        const std::vector<Tensor *> &out_tensors, const Model::Node *node,
                                        TypeId prefer_data_type = kTypeUnknown);
  int FindCpuKernel(const std::vector<Tensor *> &in_tensors, const std::vector<Tensor *> &out_tensors,
                    OpParameter *op_parameter, const kernel::KernelKey &desc, TypeId kernel_data_type,
                    kernel::LiteKernel **kernel);
  int FindGpuKernel(const std::vector<Tensor *> &in_tensors, const std::vector<Tensor *> &out_tensors,
                    OpParameter *op_parameter, const kernel::KernelKey &desc, kernel::LiteKernel **kernel);
  int FindNpuKernel(const std::vector<Tensor *> &in_tensors, const std::vector<Tensor *> &out_tensors,
                    OpParameter *op_parameter, const kernel::KernelKey &desc, kernel::LiteKernel **kernel);
  int FindProviderKernel(const std::vector<Tensor *> &in_tensors, const std::vector<Tensor *> &out_tensors,
                         const Model::Node *node, TypeId data_type, kernel::LiteKernel **kernel);

  int ReplaceDelegateKernels(std::vector<kernel::LiteKernel *> *dst_kernels);
  int InitKernels(std::vector<kernel::LiteKernel *> dst_kernels);
  kernel::LiteKernel *SchedulePartialToKernel(const lite::Model::Node *src_node);
  // schedule a partial node to a subgraph_kernel
  std::vector<kernel::LiteKernel *> ScheduleSubGraphToSubGraphKernels(const int &subgraph_index);
  // schedule a node to a kernel
  kernel::LiteKernel *ScheduleNodeToKernel(const Model::Node *src_node, TypeId prefer_data_type = kTypeUnknown);
  // schedule a Model::Graph into a vector of subgraph_kernel
  int ScheduleGraphToKernels(std::vector<kernel::LiteKernel *> *dst_kernels, TypeId prefer_data_type = kTypeUnknown);
  // schedule a Model::SubGraph into a vector of kernel and subgraph_kernel
  int ScheduleSubGraphToKernels(size_t subgraph_index, std::vector<kernel::LiteKernel *> *dst_kernels,
                                std::vector<lite::Tensor *> *in_tensors, std::vector<lite::Tensor *> *out_tensors,
                                TypeId prefer_data_type = kTypeUnknown);
  // find in_kernels_ and out_kernels of kernel, sub_graph and nodes_ in sub_graph
  static void FindAllInoutKernels(const std::vector<kernel::LiteKernel *> &kernels);
  // vector<LiteKernel/SubGraphKernel> --> vector<SubGraphKernel>
  int ConstructSubGraphs(std::vector<kernel::LiteKernel *> src_kernel, std::vector<kernel::LiteKernel *> *dst_kernel,
                         std::map<const kernel::LiteKernel *, bool> *sinked_kernel_map);
  // create subgraph_kernel from a vector of kernel
  kernel::SubGraphKernel *CreateSubGraphKernel(const std::vector<kernel::LiteKernel *> &kernels,
                                               const std::vector<lite::Tensor *> *in_tensors,
                                               const std::vector<lite::Tensor *> *out_tensors,
                                               kernel::SubGraphType type);
  bool MergeOpIsReady(const kernel::LiteKernel *kernel, std::map<const kernel::LiteKernel *, bool> is_kernel_finish);
  bool KernelFitCurrentSubGraph(const kernel::SubGraphType subgraph_type, const kernel::LiteKernel &kernel);
  std::vector<kernel::LiteKernel *> FindAllSubGraphKernels(
    std::vector<kernel::LiteKernel *> head_kernels, std::map<const kernel::LiteKernel *, bool> *sinked_kernel_map);

  // other methods
  static TypeId GetFirstFp32Fp16OrInt8Type(const std::vector<Tensor *> &in_tensors);
  static void SetKernelTensorDataType(kernel::LiteKernel *kernel);
  static kernel::SubGraphType GetKernelSubGraphType(const kernel::LiteKernel *kernel);
  int CopyPartialShapeToSubGraph(const lite::Model::Node *partial_node);
  int RestoreSubGraphInput(const lite::Model::Node *partial_node);
  bool SubGraphHasScheduled(const int &index);
  void SubGraphMarkScheduled(const int &index);
  void SetSubgraphForPartialNode();
  bool IsControlFlowPattern(const lite::Model::Node &partial_node);

 protected:
  const InnerContext *context_ = nullptr;
  Model *src_model_ = nullptr;
  std::vector<Tensor *> *src_tensors_;
  const std::vector<Tensor *> &inputs_;
  const std::vector<Tensor *> &outputs_;
  std::vector<size_t> graph_output_node_indexes_;
  std::map<int, OpParameter *> op_parameters_;
  bool is_train_session_ = false;
  std::unique_ptr<SchedulerCb> sched_cb_;
  std::map<kernel::Kernel *, const schema::Primitive *> primitives_;
  std::shared_ptr<Delegate> delegate_ = nullptr;
  std::set<int> scheduled_subgraph_index_{};
  std::deque<int> subgraphs_to_schedule_{};
  std::unordered_map<kernel::LiteKernel *, size_t> partial_kernel_subgraph_index_map_{};
  std::unordered_map<size_t, kernel::LiteKernel *> subgraph_index_subgraph_kernel_map_{};
  std::set<lite::Model::Node *> partial_cnode_inferred_{};
};
}  // namespace mindspore::lite

#endif  // MINDSPORE_LITE_SRC_SCHEDULER_H_
