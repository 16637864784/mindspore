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
#include "tools/optimizer/parallel/multi_conv_info.h"
#include <string>
#include <algorithm>
#include "tools/optimizer/parallel/spliter.h"
#include "ops/fusion/conv2d_fusion.h"
#include "tools/optimizer/parallel/split_strategy.h"

using mindspore::lite::converter::FmkType;
using mindspore::schema::PrimitiveType_Conv2dTransposeFusion;
namespace mindspore {
namespace opt {
int MultiConvSplit ::GenSplitInfo() {
  split_info_.out_num = this->strategy_.dev_num;
  for (const auto &dev_type : this->strategy_.dev_types) {
    for (const auto &support_split_device : kSupportSplitedDevices) {
      if (dev_type == support_split_device.first) {
        split_info_.dev_types.push_back(support_split_device.second);
      }
    }
  }
  if (split_info_.dev_types.empty()) {
    MS_LOG(ERROR) << "unsupported DeviceType. ";
    return RET_ERROR;
  }
  // only can get N && H && CIN &&
  std::vector<int64_t> tmp(split_info_.out_num, 0);
  for (size_t i = 0; i < this->strategy_.strategys[0].size(); i++) {
    if (this->strategy_.strategys[0][i] == tmp) {
      continue;
    }
    split_info_.axis = i;  // NHWC
    split_info_.size_splits.clear();
    split_info_.size_splits = this->strategy_.strategys[0][i];  // cal base on compute_cap
    break;
  }
  split_info_.in_num_conv = num_;
  split_info_.fmk_type = fmk_type_;
  split_info_.extend_bottom = std::vector<int64_t>(split_info_.size_splits.size(), 0);
  split_info_.extend_top = std::vector<int64_t>(split_info_.size_splits.size(), 0);
  split_info_.primitive_type = primitive_type_;
  return RET_OK;
}

int MultiConvSplit::GetMultiConvNodes(const AnfNodePtr &conv_node) {
  MS_EXCEPTION_IF_NULL(func_graph_);
  MS_EXCEPTION_IF_NULL(conv_node);
  // get nodes to be splited
  // node in graph 1->2->3...
  // node in vector ...->3->2->1
  std::string conv_cnode_name = conv_node->fullname_with_scope();
  auto graph_node_outputs = Spliter::GetInstance()->graph_node_outputs();
  auto it = graph_node_outputs.find(conv_cnode_name);
  if (it == graph_node_outputs.end()) {
    MS_LOG(ERROR) << "This node may be the last node of graph,it do not has any out-nodes.";
    return RET_ERROR;
  }
  conv_nodes_.push_back(conv_node);
  int32_t index = 0;
  while (index < split_info_.in_num_conv - 1) {
    auto curr_node = conv_nodes_[index];
    auto curr_cnode = conv_nodes_[index]->cast<CNodePtr>();
    auto tmp_node = curr_cnode->input(1);
    if (!IsConv2D(tmp_node)) {
      break;
    }
    auto name = tmp_node->fullname_with_scope();
    // check outputs's bigger than two
    it = graph_node_outputs.find(name);
    if (it == graph_node_outputs.end()) {
      return RET_ERROR;
    }
    if (it->second.size() > kDefaultBatch) {
      break;
    }
    conv_nodes_.push_back(tmp_node);
    index++;
  }
  // no need split in multi_node_pass
  if (conv_nodes_.size() < kDefaultBatch + 1) {
    return RET_ERROR;
  }
  return RET_OK;
}

AnfNodePtr MultiConvSplit::MultiConvNHSplit(const AnfNodePtr &node) {
  std::string conv_cnode_name = node->fullname_with_scope();
  // Create Split node and get outputs of Split
  std::vector<AnfNodePtr> split_outputs;
  CreateOutputsOfSplitWithOverlap(func_graph_, conv_nodes_[conv_nodes_.size() - 1], &split_outputs, &split_info_,
                                  conv_cnode_name);
  // Create Conv node
  int res_conv_numbers = static_cast<int>(conv_nodes_.size() - 1);
  for (int32_t i = res_conv_numbers; i >= 0; i--) {
    std::vector<AnfNodePtr> outputs_node;
    SplitSingleConv(conv_nodes_[i], split_outputs, {}, {}, &outputs_node);
    split_outputs.clear();
    std::copy(outputs_node.begin(), outputs_node.end(), std::back_inserter(split_outputs));
    outputs_node.clear();
  }
  // Create concate node
  auto concat_node = CreateOutputsOfConcat(func_graph_, node, split_outputs, &split_info_, conv_cnode_name);
  split_outputs.clear();
  return concat_node;
}

void MultiConvSplit::SplitSingleConv(const AnfNodePtr &ori_node, const std::vector<AnfNodePtr> &inputs_node,
                                     const std::vector<AnfNodePtr> &weight_nodes,
                                     const std::vector<AnfNodePtr> &bias_nodes, std::vector<AnfNodePtr> *outputs_node) {
  auto ori_conv_cnode = ori_node->cast<CNodePtr>();
  auto ori_attr = GetValueNode<std::shared_ptr<ops::Conv2DFusion>>(ori_conv_cnode->input(kAnfPrimitiveIndex));
  for (int64_t output_conv_index = 0; output_conv_index < (split_info_.out_num); output_conv_index++) {
    // Create Conv node attr
    auto conv_prim = CopyConvPrim(ori_attr);
    // adjust primitive
    AdJustConvPrim(conv_prim, output_conv_index);
    // node inputs
    std::vector<AnfNodePtr> conv_inputs;
    conv_inputs.push_back(NewValueNode(conv_prim));
    AdJustInputs(ori_node, inputs_node, weight_nodes, bias_nodes, output_conv_index, &conv_inputs);
    // create new conv node
    CreateNewConvNode(ori_node, conv_inputs, output_conv_index, outputs_node);
  }
}

void MultiConvSplit::AdJustInputs(const AnfNodePtr &ori_conv_node, const std::vector<AnfNodePtr> &new_inputs_node,
                                  const std::vector<AnfNodePtr> &weight_node, const std::vector<AnfNodePtr> &bias_nodes,
                                  int output_conv_index, std::vector<AnfNodePtr> *conv_inputs) {
  auto ori_conv_cnode = ori_conv_node->cast<CNodePtr>();
  // feature_map
  conv_inputs->push_back(new_inputs_node[output_conv_index]);
  // W+bias
  for (size_t j = kDefaultBatch + 1; j < ori_conv_cnode->size(); j++) {
    conv_inputs->push_back(ori_conv_cnode->input(j));
  }
}

void MultiConvSplit::CreateNewConvNode(const AnfNodePtr &ori_conv_node, const std::vector<AnfNodePtr> &conv_inputs,
                                       int output_conv_index, std::vector<AnfNodePtr> *outputs_node) {
  auto ori_conv_cnode = ori_conv_node->cast<CNodePtr>();
  std::string ori_cnode_name = ori_conv_cnode->fullname_with_scope();
  // new conv_node
  auto conv_cnode = func_graph_->NewCNode(conv_inputs);
  conv_cnode->set_fullname_with_scope(ori_cnode_name + "_" + PARALLEL_NAME_SUFFIX +
                                      std::to_string(output_conv_index + 1));
  conv_cnode->AddAttr(mindspore::ops::kDeviceType,
                      MakeValue(static_cast<int>(split_info_.dev_types[output_conv_index])));
  std::vector<AnfNodePtr> tmp_outputs;
  // conv2d only has one output, set to output_nodes
  GetMultipleOutputsOfAnfNode(func_graph_, conv_cnode, 1, &tmp_outputs);
  outputs_node->push_back(tmp_outputs[0]->cast<CNodePtr>()->input(1));
  tmp_outputs.clear();
}

AnfNodePtr MultiConvSplit::DoSplit(const FuncGraphPtr &func_graph, const AnfNodePtr &node) {
  int ret = GenSplitInfo();
  if (ret != RET_OK) {
    return node;
  }
  func_graph_ = func_graph;
  ret = GetMultiConvNodes(node);
  if (ret != RET_OK) {
    return node;
  }
  return SplitMultiConv(node);
}

AnfNodePtr MultiConvSplitN::SplitMultiConv(const AnfNodePtr &node) {
  if (conv_nodes_.size() == 2 && split_info_.axis == CuttingStragedy::CUT_N) {
    return node;
  }
  return MultiConvNHSplit(node);
}

AnfNodePtr MultiConvSplitH::SplitMultiConv(const AnfNodePtr &node) {
  // update info, N do not need, C do not support
  if (!UpdateSplitInfo(func_graph_, conv_nodes_, &split_info_)) {
    return node;
  }
  return MultiConvNHSplit(node);
}

void MultiConvSplitH::AdJustConvPrim(const std::shared_ptr<ops::Conv2DFusion> &conv_prim, int output_conv_index) {
  auto pad_list = GetSplitPadList(conv_prim);
  if (output_conv_index == 0) {
    pad_list[kPadDown] = 0;
  } else if (output_conv_index == static_cast<int>(split_info_.out_num - 1)) {
    pad_list[kPadUp] = 0;
  } else {
    pad_list[kPadUp] = 0;
    pad_list[kPadDown] = 0;
  }
  conv_prim->set_pad_list(pad_list);
}

AnfNodePtr MultiConvSplitCIN::SplitMultiConv(const AnfNodePtr &node) { return nullptr; }

AnfNodePtr MultiConvSplitCOUT::SplitMultiConv(const AnfNodePtr &node) { return nullptr; }

}  // namespace opt
}  // namespace mindspore
