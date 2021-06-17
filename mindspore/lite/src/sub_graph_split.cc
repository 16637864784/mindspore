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

#include "src/sub_graph_split.h"
#include <stdlib.h>
#include <utility>
#include <algorithm>
#include <iterator>
#include <vector>
#include "src/tensor.h"
#include "schema/ops_generated.h"
#include "schema/model_generated.h"
#include "src/ops/populate/populate_register.h"
#include "nnacl/fp32/winograd_utils.h"
#include "nnacl/pooling_parameter.h"
#if defined(ENABLE_ARM) || (defined(ENABLE_SSE) && !defined(ENABLE_AVX))
#include "nnacl/fp32/conv_depthwise_fp32.h"
#endif

namespace mindspore::lite {

size_t CommConvMul(std::vector<int> weight_shape, std::vector<int> output_shape) {
  size_t cost = output_shape[0] * output_shape[1] * output_shape[2] * output_shape[3] * weight_shape[1] *
                weight_shape[2] * weight_shape[3];
  return cost;
}

size_t WinogradConvMul() {
  /* winograd conv */
  return 0;
}

size_t CommConvdwMul(std::vector<int> weight_shape, std::vector<int> output_shape) {
  size_t cost =
    output_shape[0] * output_shape[1] * output_shape[2] * output_shape[3] * weight_shape[1] * weight_shape[2];
  return cost;
}

size_t WinogradConvDwMul() {
  /* winograd convdw */
  return 0;
}

void SearchSubGraph::dfs(int i, int n, int current_sum, int except_value, int *min_value, std::vector<bool> *tmp_group,
                         std::vector<bool> *cor_group, std::vector<Subgraph> *sub_graphs) {
  if (i == n) {
    if (abs(except_value - current_sum) < *min_value) {
      for (int i = 0; i < n; i++) {
        cor_group->at(i) = tmp_group->at(i);
      }
    }
    *min_value = MSMIN(*min_value, abs(except_value - current_sum));
    return;
  }

  {
    tmp_group->at(i) = true;
    int next_sum = current_sum + sub_graphs->at(i).cost_.cost();
    dfs(i + 1, n, next_sum, except_value, min_value, tmp_group, cor_group, sub_graphs);
  }

  {
    tmp_group->at(i) = false;
    dfs(i + 1, n, current_sum, except_value, min_value, tmp_group, cor_group, sub_graphs);
  }
  return;
}

SearchSubGraph::CostModel SearchSubGraph::CalculateConv2DFusion(Model::Node *node) {
  CostModel cost;
  std::vector<uint32_t> inputs = node->input_indices_;
  std::vector<uint32_t> outputs = node->output_indices_;

  std::vector<int> weight_shape = src_tensors_->at(inputs[1])->shape();
  std::vector<int> output_shape = src_tensors_->at(outputs[0])->shape();

  ConvParameter *param = reinterpret_cast<ConvParameter *>(op_parameters_->at(outputs[0]));

  if (param->group_ == 1) {
    if (param->kernel_h_ == 1 && param->kernel_w_ == 1) {
      size_t conv1x1_mul_cost = CommConvMul(weight_shape, output_shape);
      cost.mul_cost_ += conv1x1_mul_cost;
    } else {
      int out_unit;
      if (CheckIfUseWinograd(&out_unit, param)) {
        size_t winograd_conv_cost = WinogradConvMul();
        cost.mul_cost_ += winograd_conv_cost;
      } else {
        size_t comm_conv_mul_cost = CommConvMul(weight_shape, output_shape);
        cost.mul_cost_ += comm_conv_mul_cost;
      }
    }
  } else if (param->group_ == param->input_channel_ && param->group_ == param->output_channel_) {
#if defined(ENABLE_ARM) || (defined(ENABLE_SSE) && !defined(ENABLE_AVX))
    if (CheckConvDw1DWinograd(param, context_->thread_num_)) {
      /* ConvolutionDepthwise3x3CPUKernel */
      size_t winograd_convdw_cost = WinogradConvDwMul();
      cost.mul_cost_ += winograd_convdw_cost;
    } else {
      /* ConvolutionDepthwiseIndirectCPUKernel */
      /* ConvolutionDepthwiseSWCPUKernel */
      /* ConvolutionDepthwiseCPUKernel */
      size_t comm_convdw_cost = CommConvdwMul(weight_shape, output_shape);
      cost.mul_cost_ += comm_convdw_cost;
    }
#else
    size_t comm_convdw_cost = CommConvdwMul(weight_shape, output_shape);
    cost.mul_cost_ += comm_convdw_cost;
#endif
  } else {
    /* group conv */
  }
  return cost;
}

const schema::Primitive *SearchSubGraph::CreatePartialPrimitive(int64_t subgraph_index) {
  flatbuffers::FlatBufferBuilder fbb(1024);
  auto val_offset = schema::CreatePartialFusion(fbb, subgraph_index);
  auto prim_offset = schema::CreatePrimitive(fbb, schema::PrimitiveType_PartialFusion, val_offset.o);
  fbb.Finish(prim_offset);
  auto tmp_buf = fbb.GetBufferPointer();
  auto prim_buf = reinterpret_cast<char *>(malloc(fbb.GetSize()));
  if (prim_buf == nullptr) {
    return nullptr;
  }
  memcpy(prim_buf, tmp_buf, fbb.GetSize());

  auto primitive = flatbuffers::GetRoot<schema::Primitive>(prim_buf);
  fbb.Clear();

  model_->node_bufs_.push_back(prim_buf);
  return std::move(primitive);
}

void SearchSubGraph::ConvertSubGraphToModel(std::vector<Subgraph> *sub_graphs) {
  if (sub_graphs->size() != 2) {
    return;
  }
  Model::SubGraph *main_graphs = model_->sub_graphs_.front();

  for (Subgraph &subgraph : *sub_graphs) {
    if (subgraph.nodes_.empty()) {
      continue;
    }

    DeviceType device_type = subgraph.device_;
    size_t thread_num = subgraph.thread_;
    int new_sub_index = model_->sub_graphs_.size();
    int partial_index = model_->all_nodes_.size();

    Model::SubGraph *new_sub_graph = new (std::nothrow) Model::SubGraph();
    if (new_sub_graph == nullptr) {
      MS_LOG(ERROR) << "New sub graph failed!";
      return;
    }
    new_sub_graph->name_ = "Subgraph-split-" + std::to_string(new_sub_index);

    Model::Node *new_partial_node = new (std::nothrow) Model::Node();
    if (new_partial_node == nullptr) {
      MS_LOG(ERROR) << "New partial node failed!";
      delete new_sub_graph;
      return;
    }
    new_partial_node->name_ = "Partial-subgraph-split-" + std::to_string(new_sub_index);
    new_partial_node->node_type_ = mindspore::lite::NodeType_ValueNode;
    new_partial_node->primitive_ = CreatePartialPrimitive(new_sub_index);

    while (!subgraph.nodes_.empty()) {
      uint32_t node_index = subgraph.nodes_.front();
      Model::Node *cur_node = model_->all_nodes_[node_index];
      new_sub_graph->node_indices_.push_back(node_index);
      VectorErase(&main_graphs->node_indices_, node_index);
      VectorErase(&subgraph.nodes_, node_index);
      cur_node->device_type_ = device_type;
      op_parameters_->at(cur_node->output_indices_.at(0))->thread_num_ = thread_num;
    }

    for (uint32_t head_index : subgraph.heads_) {
      Model::Node *head_node = model_->all_nodes_[head_index];
      std::vector<uint32_t> inputs = head_node->input_indices_;
      for (auto input : inputs) {
        if (tensors_[input].type_ == CONST) {
          continue;
        }
        if (std::find(new_sub_graph->input_indices_.begin(), new_sub_graph->input_indices_.end(), input) !=
            new_sub_graph->input_indices_.end()) {
          continue;
        }
        new_sub_graph->input_indices_.insert(new_sub_graph->input_indices_.end(), input);
        new_partial_node->input_indices_.insert(new_partial_node->input_indices_.end(), input);
      }
    }

    for (uint32_t end_index : subgraph.ends_) {
      Model::Node *end_node = model_->all_nodes_[end_index];
      std::vector<uint32_t> outputs = end_node->output_indices_;
      new_sub_graph->output_indices_.insert(new_sub_graph->output_indices_.end(), outputs.begin(), outputs.end());
      new_partial_node->output_indices_.insert(new_partial_node->output_indices_.end(), outputs.begin(), outputs.end());
    }

    main_graphs->node_indices_.push_back(partial_index);
    model_->all_nodes_.push_back(std::move(new_partial_node));
    model_->sub_graphs_.push_back(std::move(new_sub_graph));
  }

  sub_graphs->clear();
  return;
}

bool SearchSubGraph::IsNodeSubGraphHead(uint32_t node_index, const std::vector<uint32_t> &ready_nodes) {
  std::vector<uint32_t> output_indexes = node_list_.at(node_index)->output_indices_;
  std::vector<uint32_t> output_nodes;
  for (uint32_t out_t : output_indexes) {
    std::vector<uint32_t> cur_nodes = tensors_[out_t].in_nodes_;
    output_nodes.insert(output_nodes.end(), cur_nodes.begin(), cur_nodes.end());
  }
  if (output_indexes.size() == 1 && output_nodes.size() == 1) {
    return false;
  }
  for (uint32_t out_n : output_nodes) {
    if (find(ready_nodes.begin(), ready_nodes.end(), out_n) == ready_nodes.end()) {
      return true;
    }
  }
  return false;
}

bool SearchSubGraph::IsNodeSubGraphHeadWithRoot(uint32_t node_index, const std::vector<uint32_t> &ready_nodes,
                                                uint32_t root_node_index) {
  std::vector<uint32_t> output_indexes = node_list_.at(node_index)->output_indices_;
  std::vector<uint32_t> output_nodes;
  for (uint32_t out_t : output_indexes) {
    std::vector<uint32_t> cur_nodes = tensors_[out_t].in_nodes_;
    output_nodes.insert(output_nodes.end(), cur_nodes.begin(), cur_nodes.end());
  }
  for (uint32_t out_n : output_nodes) {
    if (root_node_index != out_n) {
      if (find(ready_nodes.begin(), ready_nodes.end(), out_n) == ready_nodes.end()) {
        return true;
      }
    }
  }
  return false;
}

void SearchSubGraph::SearchMultyInNodes(std::vector<uint32_t> *multy_in_nodes) {
  std::vector<uint32_t> all_main_sub_nodes = model_->sub_graphs_[0]->node_indices_;

  for (size_t i = 0; i < all_main_sub_nodes.size(); i++) {
    uint32_t node_index = all_main_sub_nodes[i];
    Model::Node *node = node_list_[node_index];

    if (IsPartialNode(node->primitive_)) {
      continue;
    }

    int input_count = std::count_if(node->input_indices_.begin(), node->input_indices_.end(),
                                    [&](uint32_t in_tensor_index) { return tensors_[in_tensor_index].type_ != CONST; });

    if (input_count > 1) {
      multy_in_nodes->push_back(node_index);
    }
  }
  return;
}

void SearchSubGraph::RemoveConstNode(std::vector<uint32_t> *nodes) {
  for (int i = nodes->size() - 1; i >= 0; i--) {
    if (tensors_[nodes->at(i)].type_ == CONST) {
      VectorErase(nodes, nodes->at(i));
    }
  }
  return;
}

void SearchSubGraph::InsertNode(uint32_t index, Subgraph *subgraph) {
  if (subgraph->search_terminate_) {
    return;
  }

  Model::Node *node = node_list_.at(index);
  if (node == nullptr) {
    return;
  }

  std::vector<uint32_t> input = node->input_indices_;
  RemoveConstNode(&input);

  /* all node_input is graph_input */
  for (size_t i = 0; i < input.size(); i++) {
    if (tensors_[input[i]].type_ != INPUT) {
      break;
    }
    subgraph->heads_.clear();
    subgraph->ends_.clear();
    subgraph->nodes_.clear();
    subgraph->search_terminate_ = true;
    return;
  }

  /* split in graph */
  if (IsNodeSubGraphHead(index, subgraph->nodes_)) {
    if (subgraph->nodes_.empty()) {
      subgraph->search_terminate_ = true;
      return;
    }
    subgraph->heads_.push_back(subgraph->nodes_.front());
    return;
  }

  if (find(output_nodes_->begin(), output_nodes_->end(), index) != output_nodes_->end()) {
    subgraph->ends_.push_back(index);
  }

  /* node insert in current subgraph */
  subgraph->nodes_.insert(subgraph->nodes_.begin(), index);
  node_list_.at(index) = nullptr;

  /* search for next node */
  for (uint32_t in : input) {
    auto next_nodes = tensors_[in].out_nodes_;
    for (uint32_t next_node : next_nodes) {
      InsertNode(next_node, subgraph);
    }
  }
  return;
}

void SearchSubGraph::OptimizeAfterFusion(std::vector<Subgraph> *sub_graphs, uint32_t root_node_index) {
  MS_ASSERT(sub_graphs->size() == 2);
  for (Subgraph &sub : *sub_graphs) {
    if (sub.nodes_.empty()) {
      return;
    }
    int head_size = sub.heads_.size();
    std::vector<uint32_t> used_heads;
    for (int i = 0; i < head_size; i++) {
      uint32_t head_node_index = sub.heads_.at(i);
      if (std::find(used_heads.begin(), used_heads.end(), head_node_index) != used_heads.end()) {
        break;
      }
      std::vector<uint32_t> head_input_tensors = model_->all_nodes_[head_node_index]->input_indices_;
      RemoveConstNode(&head_input_tensors);
      if (head_input_tensors.size() != 1) continue;

      std::vector<uint32_t> input_nodes = tensors_.at(head_input_tensors.at(0)).out_nodes_;
      if (input_nodes.size() != 1) continue;
      uint32_t input_node_index = input_nodes.at(0);

      std::vector<uint32_t> input_tensors = model_->all_nodes_[input_node_index]->input_indices_;
      RemoveConstNode(&input_tensors);
      if (input_tensors.size() != 1) continue;

      /* this node qualified:
       * 1. the only input node of current head node
       * 2. all output included in current subgraph
       * 3. one input-tensor */
      if (!IsNodeSubGraphHeadWithRoot(input_node_index, sub.nodes_, root_node_index)) {
        InsertHeadNode(input_node_index, &sub);
        used_heads.push_back(head_node_index); /* delete used head at end */
      }
      head_size = sub.heads_.size();
    }
    for (auto head_index : used_heads) {
      VectorErase(&sub.heads_, head_index);
    }

    /* double check head-end node */
    /* head-end node may error after subgraph fusion  */
    for (uint32_t head_node : sub.heads_) {
      if (std::find(sub.nodes_.begin(), sub.nodes_.end(), head_node) == sub.nodes_.end()) {
        VectorErase(&sub.nodes_, head_node);
      }
    }
    for (uint32_t end_node : sub.ends_) {
      if (std::find(sub.nodes_.begin(), sub.nodes_.end(), end_node) == sub.nodes_.end()) {
        VectorErase(&sub.ends_, end_node);
      }
    }

    /* sort node index  */
    std::sort(sub.nodes_.begin(), sub.nodes_.end());
  }
}

void SearchSubGraph::InsertHeadNode(uint32_t head_node_index, Subgraph *subgraph) {
  Model::Node *node = node_list_.at(head_node_index);
  std::vector<uint32_t> head_node_inputs = node->input_indices_;
  RemoveConstNode(&head_node_inputs);

  subgraph->nodes_.push_back(head_node_index);
  node_list_.at(head_node_index) = nullptr;

  /* search for next node */
  size_t current_node_size = subgraph->nodes_.size();
  for (uint32_t in : head_node_inputs) {
    auto next_nodes = tensors_[in].out_nodes_;
    for (uint32_t next_node : next_nodes) {
      InsertNodeByMid(next_node, subgraph);
    }
  }

  if (current_node_size == subgraph->nodes_.size()) {
    subgraph->heads_.push_back(head_node_index);
  }
  return;
}

void SearchSubGraph::InsertNodeByMid(uint32_t node_index, Subgraph *subgraph) {
  Model::Node *node = node_list_.at(node_index);
  if (node == nullptr) {
    return;
  }

  auto subs_iter = node_sub_map_.find(node_index);
  if (subs_iter != node_sub_map_.end()) {
    /* node is multy-in node , already searched before */

    if (IsNodeSubGraphHead(node_index, subgraph->nodes_)) {
      /* this node can not be included in this subgraph */
      if (!subgraph->nodes_.empty()) subgraph->heads_.push_back(subgraph->nodes_.front());
      return;
    }

    subgraph->nodes_.push_back(node_index);

    /* include this multy-in-unit in current subgraph */
    std::vector<Subgraph> &subs = subs_iter->second;
    std::set<uint32_t> subs_head;
    for (Subgraph &sub : subs) {
      subgraph->nodes_.insert(subgraph->nodes_.end(), sub.nodes_.begin(), sub.nodes_.end());
      for (uint32_t head : sub.heads_) {
        subs_head.insert(head);
      }
    }

    std::set<uint32_t> subs_head_baklist = subs_head;
    for (uint32_t head_node : subs_head) {
      std::vector<uint32_t> head_input_tensors = model_->all_nodes_[head_node]->input_indices_;
      RemoveConstNode(&head_input_tensors);
      if (head_input_tensors.size() != 1) continue;
      std::vector<uint32_t> input_nodes = tensors_.at(head_input_tensors.at(0)).out_nodes_;
      if (input_nodes.size() != 1) continue;

      uint32_t input_node = input_nodes.at(0);
      if (!IsNodeSubGraphHead(input_node, subgraph->nodes_)) {
        InsertNodeByMid(input_node, subgraph);
        subs_head_baklist.erase(head_node);
      }
    }

    /* stop search  */
    for (auto head : subs_head_baklist) {
      subgraph->heads_.push_back(head);
    }
    node_sub_map_.erase(node_index);
    return;
  }

  std::vector<uint32_t> inputs = node->input_indices_;
  RemoveConstNode(&inputs);

  if (IsNodeSubGraphHead(node_index, subgraph->nodes_)) {
    if (!subgraph->nodes_.empty()) {
      uint32_t current_node_list_head = subgraph->nodes_.front();
      if (std::find(subgraph->heads_.begin(), subgraph->heads_.end(), current_node_list_head) ==
          subgraph->heads_.end()) {
        subgraph->heads_.push_back(current_node_list_head);
      }
    }
    return;
  }

  subgraph->nodes_.insert(subgraph->nodes_.begin(), node_index);
  node_list_.at(node_index) = nullptr;

  /* search for next node */
  for (uint32_t in : inputs) {
    auto next_nodes = tensors_[in].out_nodes_;
    if (next_nodes.size() == 0) {
      if (!subgraph->nodes_.empty()) subgraph->heads_.push_back(subgraph->nodes_.front());
    } else {
      for (uint32_t next_node : next_nodes) {
        InsertNodeByMid(next_node, subgraph);
      }
    }
  }
  return;
}

void SearchSubGraph::InitMiddleSubgraph(std::vector<uint32_t> *multy_in_nodes) {
  for (uint32_t node_index : *multy_in_nodes) {
    std::vector<Subgraph> node_subs;
    Model::Node *node = node_list_[node_index];
    for (uint32_t input_tensor_index : node->input_indices_) {
      Tensor *tensor = &tensors_[input_tensor_index];
      if (tensor->type_ == CONST) continue;

      std::vector<uint32_t> input_nodes = tensor->out_nodes_;
      Subgraph sub;
      sub.ends_.push_back(input_nodes[0]);
      InsertNodeByMid(input_nodes[0], &sub);
      node_subs.push_back(sub);
    }
    node_sub_map_.insert(std::make_pair(node_index, node_subs));
  }
  return;
}

void SearchSubGraph::InitSearchSubGraphByMiddle() {
  sub_graphs_.clear();
  node_list_ = model_->all_nodes_;

  std::vector<uint32_t> multy_in_nodes;

  SearchMultyInNodes(&multy_in_nodes);

  InitMiddleSubgraph(&multy_in_nodes);

  return;
}

void SearchSubGraph::InitSearchSubGraphByOutput() {
  sub_graphs_.clear();
  node_list_ = model_->all_nodes_;

  for (uint32_t out : *output_nodes_) {
    Subgraph subgraph;

    InsertNode(out, &subgraph);

    sub_graphs_.push_back(std::move(subgraph));
  }
  return;
}

void SearchSubGraph::InitSearchTensor() {
  tensors_.resize(model_->all_tensors_.size());

  /* Set Tensor Type */
  for (size_t i = 0; i < tensors_.size(); i++) {
    tensors_[i].type_ = NORMAL;
    mindspore::schema::Tensor *src_tensor = model_->all_tensors_[i];
    auto category = TensorCategory(src_tensor);
    if (category == mindspore::lite::Tensor::Category::CONST_TENSOR ||
        category == mindspore::lite::Tensor::Category::CONST_SCALAR) {
      tensors_[i].type_ = CONST;
    }
  }
  std::vector<uint32_t> graph_input = model_->sub_graphs_[0]->input_indices_;
  for (auto in : graph_input) {
    tensors_[in].type_ = INPUT;
  }

  /* Set Tensor In and out Node */
  for (size_t index = 0; index < model_->all_nodes_.size(); index++) {
    Model::Node *node = model_->all_nodes_[index];
    std::vector<uint32_t> input = node->input_indices_;
    for (uint32_t in : input) {
      tensors_[in].in_nodes_.push_back(index);
    }
    std::vector<uint32_t> output = node->output_indices_;
    for (uint32_t out : output) {
      tensors_[out].out_nodes_.push_back(index);
    }
  }
  return;
}

void SearchSubGraph::InitSubgraphRuntimeInfo(std::vector<Subgraph> *sub_graphs) {
  std::vector<bool> tmp_group;
  std::vector<bool> cor_group;

  tmp_group.resize(sub_graphs->size());
  cor_group.resize(sub_graphs->size());

  int except_value = total_cost_ * 0.5; /* major device responsible for 50% calculation */
  int min_value = INT32_MAX;

  dfs(0, sub_graphs->size(), 0, except_value, &min_value, &tmp_group, &cor_group, sub_graphs);

  /* make bigger half using major_dt_*/
  int true_value = 0;
  for (size_t i = 0; i < sub_graphs->size(); i++) {
    if (cor_group.at(i)) {
      true_value += sub_graphs->at(i).cost_.cost();
    }
  }

  if (true_value < except_value) {
    (void)std::transform(cor_group.begin(), cor_group.end(), cor_group.begin(), [](bool value) { return !value; });
  }

  for (size_t i = 0; i < sub_graphs->size(); i++) {
    if (cor_group.at(i)) {
      sub_graphs->at(i).device_ = major_dt_;
      sub_graphs->at(i).thread_ = major_thread_;
      sub_graphs->at(i).tid_ = 0;
    } else {
      sub_graphs->at(i).device_ = minor_dt_;
      sub_graphs->at(i).thread_ = minor_thread_;
      sub_graphs->at(i).tid_ = 1;
    }
  }
}

void SearchSubGraph::InitMainGraphDevice(DeviceType dt) {
  Model::SubGraph *main_graph = model_->sub_graphs_.front();
  for (uint32_t node_index : main_graph->node_indices_) {
    Model::Node *node = model_->all_nodes_[node_index];
    node->device_type_ = dt;
  }
}

void SearchSubGraph::SubgraphFusion(std::vector<Subgraph> *sub_graphs) {
  while (sub_graphs->size() > 2) {
    size_t sub1_index = 0;
    size_t sub2_index = 0;
    bool is_found = false;
    for (sub1_index = 0; sub1_index < sub_graphs->size(); sub1_index++) {
      for (size_t tmp2 = sub1_index + 1; tmp2 < sub_graphs->size(); tmp2++) {
        if (sub_graphs->at(sub1_index).tid_ == sub_graphs->at(tmp2).tid_) {
          sub2_index = tmp2;
          is_found = true;
          break;
        }
      }
      if (is_found) {
        break;
      }
    }
    MS_ASSERT(sub2_index > sub1_index); /* erase sub2 then sub1 */

    Subgraph new_sub;
    new_sub.device_ = sub_graphs->at(sub1_index).device_;
    new_sub.thread_ = sub_graphs->at(sub1_index).thread_;
    new_sub.tid_ = sub_graphs->at(sub1_index).tid_;

    Subgraph &sub1 = sub_graphs->at(sub1_index);
    Subgraph &sub2 = sub_graphs->at(sub2_index);
    new_sub.nodes_.insert(new_sub.nodes_.end(), sub1.nodes_.begin(), sub1.nodes_.end());
    new_sub.nodes_.insert(new_sub.nodes_.end(), sub2.nodes_.begin(), sub2.nodes_.end());
    new_sub.heads_.insert(new_sub.heads_.end(), sub1.heads_.begin(), sub1.heads_.end());
    new_sub.heads_.insert(new_sub.heads_.end(), sub2.heads_.begin(), sub2.heads_.end());
    new_sub.ends_.insert(new_sub.ends_.end(), sub1.ends_.begin(), sub1.ends_.end());
    new_sub.ends_.insert(new_sub.ends_.end(), sub2.ends_.begin(), sub2.ends_.end());
    sub_graphs->erase(sub_graphs->begin() + sub2_index);
    sub_graphs->erase(sub_graphs->begin() + sub1_index);
    sub_graphs->insert(sub_graphs->end(), std::move(new_sub));
  }

  return;
}

void SearchSubGraph::CalculateCostModel(std::vector<Subgraph> *sub_graphs) {
  total_cost_ = 0;
  for (Subgraph &subgraph : *sub_graphs) {
    subgraph.cost_.empty();
    std::vector<uint32_t> nodes = subgraph.nodes_;
    for (uint32_t node_index : nodes) {
      CostModel cost;
      cost.io_cost_ = 0;
      cost.mul_cost_ = 1;

      Model::Node *node = model_->all_nodes_[node_index];
      if (GetPrimitiveType(node->primitive_) == schema::PrimitiveType_Conv2DFusion) {
        cost = CalculateConv2DFusion(node);
      }

      subgraph.cost_ = subgraph.cost_ + cost;
      total_cost_ += cost.cost();
    }
  }
}

void SearchSubGraph::SubGraphSplitByOutput() {
  InitSearchSubGraphByOutput();
  CalculateCostModel(&sub_graphs_);
  InitSubgraphRuntimeInfo(&sub_graphs_);
  SubgraphFusion(&sub_graphs_);
  ConvertSubGraphToModel(&sub_graphs_);
}

void SearchSubGraph::SubGraphSplitByMiddle() {
  InitSearchSubGraphByMiddle();
  for (auto map : node_sub_map_) {
    std::vector<Subgraph> &subgraphs = map.second;

    CalculateCostModel(&subgraphs);
    InitSubgraphRuntimeInfo(&subgraphs);
    SubgraphFusion(&subgraphs);

    MS_ASSERT(subgraphs.size() == 2);
    if (std::any_of(subgraphs.begin(), subgraphs.end(), [&](Subgraph &sub) { return sub.nodes_.empty(); })) {
      continue;
    }

    OptimizeAfterFusion(&subgraphs, map.first);

    /* redo cost-model and pre-set-info after optimize */
    CalculateCostModel(&subgraphs);
    if (subgraphs.at(0).cost_.cost() == 0 || subgraphs.at(1).cost_.cost() == 0) {
      continue;
    }

    InitSubgraphRuntimeInfo(&subgraphs);

    InitMainGraphDevice(DT_CPU);

    ConvertSubGraphToModel(&subgraphs);
  }
}

SearchSubGraph::SearchSubGraph(const InnerContext *context, Model *model, std::vector<lite::Tensor *> *src_tensors,
                               const std::map<int, OpParameter *> *op_parameters, std::vector<size_t> *output_nodes)
    : output_nodes_(output_nodes), context_(context), src_tensors_(src_tensors), op_parameters_(op_parameters) {
  model_ = reinterpret_cast<LiteModel *>(model);

  major_dt_ = DT_CPU;
  minor_dt_ = DT_CPU;
  if (context_->IsNpuEnabled()) {
    major_dt_ = DT_NPU;
  } else if (context_->IsGpuEnabled()) {
    major_dt_ = DT_GPU;
  }

  if (major_dt_ == DT_GPU) {
    major_thread_ = 1;
    minor_thread_ = context_->thread_num_ - 1;
  } else if (major_dt_ == DT_CPU) {
    major_thread_ = UP_DIV(context_->thread_num_, 2);
    minor_thread_ = context_->thread_num_ - major_thread_;
  }
  MS_ASSERT(major_thread_ > 0);
  MS_ASSERT(minor_thread_ > 0);

  InitSearchTensor();
  return;
}

void SearchSubGraph::InsertParallelNode(uint32_t index, Subgraph *subgraph) {
  if (subgraph == nullptr) {
    return;
  }
  if (subgraph->search_terminate_) {
    return;
  }
  Model::Node *node = node_list_[index];
  //  has been searched
  if (node == nullptr) {
    return;
  }
  // just deal with parallel target node
  std::vector<uint32_t> input = node->input_indices_;
  /* remove const node */
  for (int i = static_cast<int>(input.size()) - 1; i >= 0; i--) {
    if (tensors_[input[i]].type_ == CONST) {
      VectorErase(&input, input[i]);
    }
  }
  // search to graph to graph input , terminate it.
  if (std::any_of(input.begin(), input.end(), [&](int input_index) { return tensors_[input_index].type_ == INPUT; })) {
    subgraph->search_terminate_ = true;
    return;
  }
  // if current node is no parallel target node, just judge terminate or continue
  if (GetPrimitiveType(node->primitive_) == schema::PrimitiveType_Conv2DFusion &&
      node->device_type_ != kDefaultDeviceType) {
    // first searched
    if (subgraph->nodes_.empty()) {
      subgraph->device_ = static_cast<DeviceType>(node->device_type_);
    } else {
      // check pre_device_type equal to current device_type
      if (subgraph->device_ != static_cast<DeviceType>(node->device_type_)) {
        return;
      }
    }
    if (IsNodeSubGraphHead(index, subgraph->nodes_)) {
      if (subgraph->nodes_.empty()) {
        subgraph->search_terminate_ = true;
        return;
      }
      subgraph->heads_.push_back(subgraph->nodes_.front());
      return;
    }

    // for offline parallel target subgraph only has one end
    if (subgraph->ends_.empty()) {
      subgraph->ends_.push_back(index);
    }

    subgraph->nodes_.insert(subgraph->nodes_.begin(), index);
    node_list_[index] = nullptr;
  } else {
    if (!subgraph->nodes_.empty()) {
      return;
    }
  }

  // search for next nodes
  for (uint32_t next : input) {
    auto next_nodes = tensors_[next].out_nodes_;
    for (uint32_t next_node : next_nodes) {
      InsertParallelNode(next_node, subgraph);
    }
  }
}

void SearchSubGraph::InitSearchParallelSubGraph() {
  // for every graph output, find a parallel subgraph
  for (uint32_t output : *output_nodes_) {
    Subgraph subgraph;
    InsertParallelNode(output, &subgraph);
    sub_graphs_.push_back(std::move(subgraph));
  }
}

void SearchSubGraph::SubGraphSplitByOffLineParallel() {
  MS_LOG(DEBUG) << "start to split offline parallel subgraph";
  InitSearchParallelSubGraph();
  ConvertSubGraphToModel(&sub_graphs_);
  InitMainGraphDevice();
  MS_LOG(DEBUG) << "end to split offline parallel subgraph";
}

void SearchSubGraph::SubGraphSplit() {
  if (offline_parallel_enable_) {
    SubGraphSplitByOffLineParallel();
  } else {
    SubGraphSplitByOutput();
    SubGraphSplitByMiddle();
  }
  return;
}
}  // namespace mindspore::lite
