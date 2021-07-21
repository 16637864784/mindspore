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

#include "tools/optimizer/parallel/parallel_pass.h"
#include "include/errorcode.h"
#include "ir/tensor.h"
#include "tools/optimizer/parallel/operator_info_register.h"
#include "ops/fusion/conv2d_fusion.h"

namespace mindspore {
namespace opt {

namespace {
constexpr auto kAnfPrimitiveIndex = 0;
}

bool ParallelPass::IsParallelCareNode(const AnfNodePtr &node) {
  auto c_node = node->cast<CNodePtr>();
  auto prim = GetValueNode<PrimitivePtr>(c_node->input(kAnfPrimitiveIndex));
  // depth_wise can not be splited in conv_info, we deal with in depthwise_conv_info
  is_depth_wise_ = prim->GetAttr(ops::kIsDepthWise) != nullptr && GetValue<bool>(prim->GetAttr(ops::kIsDepthWise));
  type_name_.clear();
  return std::any_of(kParallelOpNames.begin(), kParallelOpNames.end(), [this, &node](auto &prim_item) {
    if (CheckPrimitiveType(node, prim_item.first.first) && is_depth_wise_ == prim_item.first.second) {
      type_name_ = prim_item.second;
    }
    return !type_name_.empty();
  });
}

bool ParallelPass::SetParallelOpName(const AnfNodePtr &node, std::string *parallel_name) {
  if (!utils::isa<CNode>(node)) {
    return false;
  }
  auto cnode = node->cast<CNodePtr>();
  std::string cnode_name = cnode->fullname_with_scope();
  if (cnode_name.find(PARALLEL_NAME_SUFFIX) != std::string::npos) {
    MS_LOG(DEBUG) << " : Skip splited cnode " << cnode_name;
    return false;
  }

  // find operator name first, then operator type name.
  if (split_strategys_.find(*parallel_name) == split_strategys_.end()) {
    *parallel_name = type_name_;
  }

  MS_LOG(DEBUG) << " : Reached a parallel care node: " << cnode_name;
  if (split_strategys_.find(*parallel_name) == split_strategys_.end()) {
    MS_LOG(DEBUG) << *parallel_name << " : No split strategy for the current CNode.";
    return false;
  }
  cnode->set_fullname_with_scope(cnode_name + PARALLEL_NAME_SUFFIX);
  return true;
}

OperatorInfoPtr ParallelPass::CreateParallelOperator(const AnfNodePtr &node, const std::string &scope_name,
                                                     const std::string &parallel_op_name) {
  // foreach kernel_list && data_type
  auto cnode = node->cast<CNodePtr>();
  auto node_prim = cnode->input(kParallelPrimitiveIndex);
  auto prim = GetValueNode<PrimitivePtr>(node_prim);
  for (const auto &schmea_id : kParallelSchemaId) {
    if (!CheckPrimitiveType(node, schmea_id.first)) {
      continue;
    }
    auto split_key_pair = kParallelSchemaId.find(schmea_id.first);
    auto split_schema_id = split_key_pair->second.first;
    auto split_type_id = split_key_pair->second.second;
    SplitOpKey op_key = SplitOpKey(split_schema_id, split_type_id, is_depth_wise_);
    auto op_create_func = OperatorInfoFactory::GeInstance()->FindOperatorInfo(op_key);
    if (op_create_func == nullptr) {
      return nullptr;
    }
    OperatorInfoPtr op = op_create_func(scope_name, split_strategys_[parallel_op_name]);
    return op;
  }
  return nullptr;
}

AnfNodePtr ParallelPass::Run(const FuncGraphPtr &func_graph, const AnfNodePtr &node) {
  if (CheckIfFuncGraphIsNull(func_graph) != RET_OK || CheckIfAnfNodeIsNull(node) != RET_OK) {
    return node;
  }
  if (!utils::isa<CNode>(node)) {
    return node;
  }
  if (!IsParallelCareNode(node)) {
    return node;
  }
  // if current conv2d node has two output nodes ,we do not split it;
  auto manager = func_graph->manager();
  auto iter = manager->node_users().find(node);
  if (iter == manager->node_users().end()) {
    MS_LOG(ERROR) << "node : " << node->fullname_with_scope() << "has no output";
  }
  auto output_info_list = iter->second;
  if (output_info_list.size() > kDefaultBatch) {
    return node;
  }
  auto cnode = node->cast<CNodePtr>();
  if (CheckIfCNodeIsNull(cnode) != RET_OK) {
    return node;
  }

  std::string parallel_op_name = cnode->fullname_with_scope();
  if (!SetParallelOpName(node, &parallel_op_name)) {
    return node;
  }

  std::string cnode_name = cnode->fullname_with_scope();
  OperatorInfoPtr parallel_operator = CreateParallelOperator(node, cnode_name, parallel_op_name);
  if (parallel_operator == nullptr) {
    MS_LOG(ERROR) << "Failure: Create " << parallel_op_name << " OperatorInstance failed";
    return node;
  }
  parallel_operator->Init(func_graph, cnode, fmk_type_);
  if (parallel_operator->DoSplit() == RET_ERROR) {
    MS_LOG(ERROR) << "Failure: operator " << parallel_op_name << " init failed";
    return node;
  }
  return parallel_operator->replace_op();
}
}  // namespace opt
}  // namespace mindspore
