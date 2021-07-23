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

#include "tools/optimizer/graph/redundant_op_remove_pass.h"
#include <memory>
#include <vector>
#include <utility>
#include "include/errorcode.h"
#include "tools/anf_exporter/fetch_content.h"
#include "tools/converter/ops/ops_def.h"
#include "ops/depend.h"
#include "ops/fusion/pad_fusion.h"
#include "ops/op_utils.h"

namespace mindspore::opt {
namespace {
int ProcessInputIsMonad(const FuncGraphPtr &func_graph, const CNodePtr &cnode) {
  MS_ASSERT(func_graph != nullptr && cnode != nullptr);
  auto first_input = cnode->input(1);
  auto second_input = cnode->input(kInputIndexTwo);
  AnfNodePtr must_monad = nullptr;
  AnfNodePtr not_must_monad = nullptr;
  if (utils::isa<ValueNode>(first_input)) {
    auto value_node = first_input->cast<ValueNodePtr>();
    MS_ASSERT(value_node->value() != nullptr);
    if (utils::isa<Monad>(value_node->value())) {
      must_monad = first_input;
      not_must_monad = second_input;
    }
  }
  if (utils::isa<ValueNode>(second_input)) {
    auto value_node = second_input->cast<ValueNodePtr>();
    MS_ASSERT(value_node->value() != nullptr);
    if (utils::isa<Monad>(value_node->value())) {
      must_monad = second_input;
      not_must_monad = first_input;
    }
  }
  if (must_monad == nullptr) {
    return lite::RET_NO_CHANGE;
  }
  auto manager = func_graph->manager();
  MS_ASSERT(manager != nullptr);
  if (!utils::isa<CNode>(not_must_monad) || CheckIsAllInputsParam(not_must_monad)) {
    manager->Replace(cnode, must_monad);
  } else {
    manager->Replace(cnode, not_must_monad);
  }
  return lite::RET_OK;
}

int ProcessDependencyWithTwoNodes(const FuncGraphPtr &func_graph, const CNodePtr &cnode, bool pre_node_is_first) {
  MS_ASSERT(func_graph != nullptr && cnode != nullptr);
  AnfNodePtr pre_node = cnode->input(1);
  AnfNodePtr post_node = cnode->input(kInputIndexTwo);
  if (!pre_node_is_first) {
    pre_node = cnode->input(kInputIndexTwo);
    post_node = cnode->input(1);
  }
  auto manager = func_graph->manager();
  MS_ASSERT(manager != nullptr);
  auto node_users = manager->node_users()[pre_node];
  auto iter =
    std::find_if(node_users.begin(), node_users.end(),
                 [&post_node](const std::pair<AnfNodePtr, int> &post_pair) { return post_pair.first == post_node; });
  if (iter == node_users.end()) {
    return lite::RET_NO_CHANGE;
  }
  auto tr = manager->Transact();
  tr.SetEdge(post_node, iter->second, NewValueNode(std::make_shared<UMonad>()));
  tr.Commit();
  auto depend_prim = std::make_shared<ops::Depend>();
  auto depend_node = func_graph->NewCNode(depend_prim, {post_node, pre_node});
  depend_node->set_fullname_with_scope(cnode->fullname_with_scope());
  manager->Replace(cnode, depend_node);
  return lite::RET_OK;
}

int ProcessInputHaveDependency(const FuncGraphPtr &func_graph, const CNodePtr &cnode) {
  MS_ASSERT(func_graph != nullptr && cnode != nullptr);
  if (ProcessDependencyWithTwoNodes(func_graph, cnode, true) == lite::RET_OK) {
    return lite::RET_OK;
  }
  if (ProcessDependencyWithTwoNodes(func_graph, cnode, false) == lite::RET_OK) {
    return lite::RET_OK;
  }
  auto make_tuple_prim = NewValueNode(std::make_shared<lite::MakeTuple>());
  auto manager = func_graph->manager();
  MS_ASSERT(manager != nullptr);
  manager->Replace(cnode->input(0), make_tuple_prim);
  return lite::RET_OK;
}
}  // namespace

int RemoveRedundantOpPass::ReplaceOp(const AnfNodePtr &anf_node, const FuncGraphManagerPtr &manager) {
  if (!utils::isa<CNodePtr>(anf_node)) {
    MS_LOG(DEBUG) << "anf node is node a cnode.";
    return lite::RET_NO_CHANGE;
  }
  auto cnode = anf_node->cast<CNodePtr>();
  if (CheckPrimitiveType(anf_node, kPrimIdentity)) {
    if (cnode->size() != kInputSizeTwo) {
      MS_LOG(DEBUG) << "The node inputs size is bigger than 1";
      remove_cnode_.insert(anf_node);
      return lite::RET_NO_CHANGE;
    }
  }
  if (CheckPrimitiveType(anf_node, prim::kPrimDepend)) {
    if (cnode->size() != kInputSizeTwo) {
      MS_LOG(DEBUG) << "The node inputs size is bigger than 1";
      remove_cnode_.insert(anf_node);
      return lite::RET_NO_CHANGE;
    }
  }
  if (CheckPrimitiveType(anf_node, prim::kPrimTranspose)) {
    if (cnode->size() != kInputSizeThree) {
      MS_LOG(DEBUG) << "The node inputs size is bigger than 2";
      remove_cnode_.insert(anf_node);
      return lite::RET_NO_CHANGE;
    }
  }

  bool replace_succ = manager->Replace(anf_node, cnode->input(1));
  if (!replace_succ) {
    MS_LOG(ERROR) << "replace redundant op failed.";
    return lite::RET_ERROR;
  }
  return RET_OK;
}

int RemoveRedundantOpPass::ReplaceUpdateStateOp(const FuncGraphPtr &func_graph, const AnfNodePtr &anf_node) {
  if (!utils::isa<CNodePtr>(anf_node)) {
    MS_LOG(DEBUG) << "anf node is node a cnode.";
    return lite::RET_NO_CHANGE;
  }
  auto cnode = anf_node->cast<CNodePtr>();
  if (ProcessInputIsMonad(func_graph, cnode) == lite::RET_OK) {
    return lite::RET_OK;
  }
  // both of two inputs are not monad, but have dependency.
  return ProcessInputHaveDependency(func_graph, cnode);
}

int RemoveRedundantOpPass::ReplaceTupleGetItem(const AnfNodePtr &anf_node, const FuncGraphManagerPtr &manager) {
  if (!utils::isa<CNodePtr>(anf_node)) {
    MS_LOG(DEBUG) << "anf node is node a cnode.";
    return lite::RET_NO_CHANGE;
  }
  if (!CheckPrimitiveType(anf_node, prim::kPrimTupleGetItem)) {
    return lite::RET_NO_CHANGE;
  }
  auto cnode = anf_node->cast<CNodePtr>();
  if (cnode->inputs().size() != kInputSizeThree) {
    MS_LOG(ERROR) << "TupleGetItem should have 3 inputs, got " << cnode->inputs().size();
    return RET_ERROR;
  }
  if (!CheckPrimitiveType(cnode->input(1), kPrimIdentity)) {
    return lite::RET_NO_CHANGE;
  }
  auto get_item_input_cnode = cnode->input(1)->cast<CNodePtr>();
  auto index_vnode = cnode->input(kInputIndexTwo);
  if (!utils::isa<ValueNode>(index_vnode)) {
    MS_LOG(ERROR) << "TupleGetItem's input 2 is not valuenode";
    return lite::RET_ERROR;
  }
  int index = CastToInt(index_vnode->cast<ValueNodePtr>()->value()).front();
  int input_cnode_inputs_size = get_item_input_cnode->inputs().size();
  if ((index + 1) >= input_cnode_inputs_size) {
    MS_LOG(ERROR) << "value node index is out of range.";
    return lite::RET_ERROR;
  }
  bool replace_succ = manager->Replace(anf_node, get_item_input_cnode->input(index + 1));
  if (!replace_succ) {
    MS_LOG(ERROR) << "replace identity failed.";
    return lite::RET_ERROR;
  }
  return lite::RET_OK;
}

int RemoveRedundantOpPass::RemoveDropoutOp(const AnfNodePtr &anf_node, const FuncGraphManagerPtr &manager) {
  MS_ASSERT(anf_node != nullptr);
  MS_ASSERT(manager != nullptr);
  if (!utils::isa<CNodePtr>(anf_node)) {
    MS_LOG(DEBUG) << "anf node is node a cnode.";
    return lite::RET_NO_CHANGE;
  }
  auto cnode = anf_node->cast<CNodePtr>();
  if (cnode->size() > kInputSizeTwo) {
    MS_LOG(ERROR) << "dropout input invalid.";
    return lite::RET_ERROR;
  }
  if (!utils::isa<abstract::AbstractTuplePtr>(anf_node->abstract())) {
    MS_LOG(DEBUG) << "dropout output size is one.";
    manager->Replace(anf_node, cnode->input(1));
  } else {
    auto node_users = manager->node_users()[anf_node];
    for (auto &node_user : node_users) {
      auto node = node_user.first;
      if (!CheckPrimitiveType(node, prim::kPrimTupleGetItem)) {
        MS_LOG(ERROR) << "dropout out node is invalid.";
        return lite::RET_ERROR;
      }
      auto get_index_node = node->cast<CNodePtr>()->input(kInputIndexTwo)->cast<ValueNodePtr>();
      if (get_index_node == nullptr) {
        MS_LOG(ERROR) << "tuple get item node is invalid.";
        return lite::RET_ERROR;
      }
      auto get_index = CastToInt(get_index_node->value()).front();
      if (get_index > 0 && !manager->node_users()[node].empty()) {
        MS_LOG(ERROR) << "dropout's second output is useful.";
        return lite::RET_ERROR;
      }
      manager->Replace(node, cnode->input(1));
    }
  }
  return lite::RET_OK;
}

int RemoveRedundantOpPass::RemoveInvalidPadOp(const AnfNodePtr &anf_node, const FuncGraphManagerPtr &manager) {
  if (!utils::isa<CNodePtr>(anf_node)) {
    MS_LOG(DEBUG) << "anf node is node a cnode.";
    return lite::RET_NO_CHANGE;
  }
  auto cnode = anf_node->cast<CNodePtr>();
  auto primitive = GetValueNode<mindspore::PrimitivePtr>(cnode->input(0));
  if (primitive == nullptr) {
    MS_LOG(ERROR) << "primitive is nullptr:" << cnode->fullname_with_scope();
    return lite::RET_NO_CHANGE;
  }
  auto is_invalid = true;
  if (cnode->size() > kInputSizeTwo) {
    auto padding_node = cnode->input(kInputIndexTwo);
    lite::DataInfo data_info;
    if (utils::isa<Parameter>(padding_node)) {
      auto status =
        lite::FetchDataFromParameterNode(cnode, kInputIndexTwo, lite::converter::FmkType_MS, false, &data_info);
      if (status != lite::RET_OK && status != lite::RET_NO_CHANGE) {
        MS_LOG(ERROR) << "fetch data from parameter node failed.";
        return lite::RET_ERROR;
      }
    } else if (utils::isa<ValueNode>(padding_node)) {
      auto status = lite::FetchDataFromValueNode(cnode, kInputIndexTwo, lite::converter::FmkType_MS, false, &data_info);
      if (status != lite::RET_OK && status != lite::RET_NO_CHANGE) {
        MS_LOG(ERROR) << "fetch data from value node failed.";
        return lite::RET_ERROR;
      }
    }
    if (!data_info.data_.empty()) {
      auto pad_data = reinterpret_cast<int *>(data_info.data_.data());
      size_t num = data_info.data_.size() / sizeof(int);
      for (size_t i = 0; i < num; ++i) {
        if (pad_data[i] != 0) {
          is_invalid = false;
          break;
        }
      }
    } else {
      is_invalid = false;
    }
  } else {
    auto pad_prim = utils::cast<std::shared_ptr<mindspore::ops::PadFusion>>(primitive);
    if (pad_prim->GetAttr(ops::kPadding) != nullptr) {
      auto pad_data = pad_prim->get_paddings();
      for (size_t i = 0; i < pad_data.size(); i++) {
        for (size_t j = 0; j < pad_data[i].size(); j++) {
          if (pad_data[i][j] != 0) {
            is_invalid = false;
            break;
          }
        }
        if (is_invalid == false) {
          break;
        }
      }
    }
  }
  if (is_invalid) {
    return ReplaceOp(anf_node, manager);
  }
  return lite::RET_OK;
}

int RemoveRedundantOpPass::RemoveInvalidTransposeOp(const AnfNodePtr &anf_node, const FuncGraphManagerPtr &manager) {
  auto cnode = anf_node->cast<CNodePtr>();
  if (cnode->size() != kInputSizeThree) {
    MS_LOG(DEBUG) << "The node inputs size is bigger than 2";
    return lite::RET_NO_CHANGE;
  }
  auto index_node = cnode->inputs()[kInputIndexTwo]->cast<ParameterPtr>();
  if (index_node == nullptr) {
    return RET_OK;
  }
  auto tensor_info = std::dynamic_pointer_cast<tensor::Tensor>(index_node->default_param());
  if (tensor_info->Size() != 0) {
    return RET_OK;
  }
  return ReplaceOp(anf_node, manager);
}

bool RemoveRedundantOpPass::Run(const FuncGraphPtr &func_graph) {
  MS_ASSERT(func_graph != nullptr);
  auto manager = func_graph->manager();
  MS_ASSERT(manager != nullptr);
  auto node_list = TopoSort(func_graph->get_return());
  int status = RET_OK;
  for (auto &node : node_list) {
    if (!utils::isa<CNodePtr>(node)) {
      continue;
    }
    if (CheckPrimitiveType(node, kPrimIdentity)) {
      status = ReplaceOp(node, manager);
    }
    if (CheckPrimitiveType(node, prim::kPrimLoad)) {
      status = ReplaceOp(node, manager);
    }
    if (CheckPrimitiveType(node, prim::kPrimUpdateState)) {
      status = ReplaceUpdateStateOp(func_graph, node);
    }
    if (CheckPrimitiveType(node, prim::kPrimTupleGetItem)) {
      status = ReplaceTupleGetItem(node, manager);
    }
    if (CheckPrimitiveType(node, prim::kPrimDropout)) {
      status = RemoveDropoutOp(node, manager);
    }
    if (CheckPrimitiveType(node, prim::kPrimPadFusion)) {
      status = RemoveInvalidPadOp(node, manager);
    }
    if (CheckPrimitiveType(node, prim::kPrimTranspose)) {
      status = RemoveInvalidTransposeOp(node, manager);
    }
    if (CheckPrimitiveType(node, prim::kPrimIf) || CheckPrimitiveType(node, prim::kPrimWhile)) {
      auto sub_func_graph = GetValueNode<FuncGraphPtr>(node->cast<CNodePtr>()->input(1));
      if (sub_func_graph == nullptr) {
        lite::ReturnCode::GetSingleReturnCode()->UpdateReturnCode(lite::RET_NULL_PTR);
        return false;
      }
      (void)Run(sub_func_graph);
      sub_func_graph = GetValueNode<FuncGraphPtr>(node->cast<CNodePtr>()->input(kInputIndexTwo));
      if (sub_func_graph == nullptr) {
        lite::ReturnCode::GetSingleReturnCode()->UpdateReturnCode(lite::RET_NULL_PTR);
        return false;
      }
      (void)Run(sub_func_graph);
    }
    if (status != lite::RET_OK && status != lite::RET_NO_CHANGE) {
      MS_LOG(ERROR) << "remove identity pass is failed.";
      return false;
    }
  }
  for (auto &node : remove_cnode_) {
    func_graph->DropNode(node);
  }
  return true;
}
}  // namespace mindspore::opt
