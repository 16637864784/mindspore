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

#include "pipeline/jit/static_analysis/order_enforce.h"
#include <algorithm>
#include <map>
#include <queue>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include "base/core_ops.h"

namespace mindspore::pipeline {
namespace {
class OrderEnforcer {
 public:
  explicit OrderEnforcer(const FuncGraphPtr &func_graph) : func_graph_(func_graph), manager_(func_graph->manager()) {
    MS_EXCEPTION_IF_NULL(func_graph_);
    MS_EXCEPTION_IF_NULL(manager_);
  }
  ~OrderEnforcer() = default;

  void Run() {
    auto nodes = MakeTopoSortMap();
    for (auto &node : nodes) {
      if (IsPrimitiveCNode(node, prim::kPrimUpdateState)) {
        HandleUpdateState(node);
      } else if (IsPrimitiveCNode(node, prim::kPrimMakeTuple)) {
        // op(MakTuple(Load, ...)) sometimes do not attach update_state,
        // So need special treatment in order to ensure the exec_order of MakeTuple users.
        HandleMakeTupleUsers(node);
      }
    }
  }

 private:
  AnfNodePtrList MakeTopoSortMap() {
    auto nodes = TopoSort(func_graph_->get_return());
    for (size_t i = 0; i < nodes.size(); ++i) {
      topo_sort_map_.emplace(nodes[i], i);
    }
    return nodes;
  }

  void HandleUpdateState(const AnfNodePtr &node) {
    auto update_state = node->cast<CNodePtr>();
    MS_EXCEPTION_IF_NULL(update_state);
    const size_t update_state_inputs_size = 3;
    if (update_state->inputs().size() < update_state_inputs_size) {
      MS_LOG(ERROR) << "UpdateState inputs size is less than 3, node is:" << update_state->DebugString();
    }
    if (!HasAbstractUMonad(update_state->input(1))) {
      // Skip UpdateStates for IO.
      return;
    }
    const size_t attach_index = 2;
    auto &attach = update_state->input(attach_index);
    if (IsPrimitiveCNode(attach, prim::kPrimLoad) || IsPrimitiveCNode(attach, prim::kPrimMakeTuple)) {
      return;
    } else if (attach->isa<CNode>()) {
      EnforceOrderForOtherCNode(attach->cast<CNodePtr>());
    }
  }

  bool CheckMakeTupleHaveLoad(const CNodePtr &cnode) {
    auto inputs = cnode->inputs();
    for (size_t index = 1; index < inputs.size(); index++) {
      auto input = cnode->input(index);
      if (IsPrimitiveCNode(input, prim::kPrimLoad)) {
        return true;
      }
    }
    return false;
  }

  std::vector<AnfNodePtr> FindUpdateStateUsers(const CNodePtr &cnode) {
    auto &node_users = manager_->node_users();
    auto iter = node_users.find(cnode);
    if (iter == node_users.end()) {
      return {};
    }
    std::vector<AnfNodePtr> update_states;
    auto &users = iter->second;
    for (auto &user : users) {
      auto &user_node = user.first;
      if (IsPrimitiveCNode(user_node, prim::kPrimUpdateState)) {
        update_states.emplace_back(user_node);
      } else if (IsPrimitiveCNode(user_node, prim::kPrimMakeTuple)) {
        auto make_tuple_users = FindUpdateStateUsers(user_node->cast<CNodePtr>());
        for (auto make_tuple_user : make_tuple_users) {
          if (IsPrimitiveCNode(make_tuple_user, prim::kPrimUpdateState)) {
            update_states.emplace_back(make_tuple_user);
          }
        }
      }
    }
    return update_states;
  }

  AnfNodePtr FindLastUpdateState(const CNodePtr &cnode) {
    auto inputs = cnode->inputs();
    std::vector<AnfNodePtr> all_update_states;
    for (size_t index = 1; index < inputs.size(); index++) {
      auto input = cnode->input(index);
      if (IsPrimitiveCNode(input, prim::kPrimLoad)) {
        std::vector<AnfNodePtr> update_states = FindUpdateStateUsers(input->cast<CNodePtr>());
        std::copy(update_states.begin(), update_states.end(), std::back_inserter(all_update_states));
      }
    }
    AnfNodePtr last_update_state = nullptr;
    if (all_update_states.empty()) {
      return last_update_state;
    }
    if (all_update_states.size() == 1) {
      return all_update_states[0];
    }
    for (size_t i = 0; i < all_update_states.size() - 1; i++) {
      auto cur_update_state = all_update_states[i];
      auto next_update_state = all_update_states[i + 1];
      if (topo_sort_map_[cur_update_state] <= topo_sort_map_[next_update_state]) {
        last_update_state = next_update_state;
      }
    }
    return last_update_state;
  }

  // Convert:
  // load1 = Load(para1, u1)
  // load2 = Load(para2, u2)
  // maketuple1 = MakeTuple(inputs, load1, load2)
  // addn = AddN(maketupe1) or other-op
  // maketuple2 = MakeTuple(load1, load2)
  // u3 = UpdateState(u', maketuple2)
  // assign = Assign(para2, inputs, u3)
  // To:
  // load1 = Load(para1, u1)
  // load2 = Load(para2, u2)
  // maketuple1 = MakeTuple(inputs, load1, load2)
  // addn = AddN(maketupe1) or other-op
  // maketuple2 = MakeTuple(load1, load2)
  // u3 = UpdateState(u', maketuple2, addn) # need put addn or other-op into u3 inputs
  // assign = Assign(para2, inputs, u3)
  void HandleMakeTupleUsers(const AnfNodePtr &node) {
    auto maketuple = node->cast<CNodePtr>();
    MS_EXCEPTION_IF_NULL(maketuple);
    if (CheckMakeTupleHaveLoad(maketuple)) {
      auto update_state = FindLastUpdateState(maketuple);
      if (update_state != nullptr) {
        std::unordered_set<AnfNodePtr> maketuple_users = GetSpecialOperatorRealUsers(maketuple);
        auto update_state_cnode = update_state->cast<CNodePtr>();
        MS_EXCEPTION_IF_NULL(update_state_cnode);
        AddInputEdges(update_state_cnode, maketuple_users);
      }
    }
  }

  bool IsRef(const AnfNodePtr &node) {
    auto &abs = node->abstract();
    return abs != nullptr && abs->isa<abstract::AbstractRef>();
  }

  // Find Load or parameter users as the candidate nodes to enforce order of execution.
  std::unordered_set<AnfNodePtr> GetSpecialOperatorRealUsers(const AnfNodePtr &node) {
    auto &node_users = manager_->node_users();
    auto iter = node_users.find(node);
    if (iter == node_users.end()) {
      return {};
    }
    std::unordered_set<AnfNodePtr> real_users;
    auto &users = iter->second;
    for (auto &user : users) {
      auto &user_node = user.first;
      real_users.insert(user_node);
    }
    return real_users;
  }

  bool IsOneOfPrimitive(const AnfNodePtr &node, const std::set<PrimitivePtr> &special_node_types) const {
    for (const auto &type : special_node_types) {
      if (IsPrimitiveCNode(node, type)) {
        return true;
      }
    }
    return false;
  }

  void EnforceOrderForOtherCNode(const CNodePtr &cnode) {
    // Find refs from the cnode inputs.
    auto &inputs = cnode->inputs();
    const size_t last_index = inputs.size() - 1;
    auto last_input = cnode->input(last_index);
    if (!IsPrimitiveCNode(last_input, prim::kPrimUpdateState)) {
      return;
    }
    const std::set<PrimitivePtr> special_operators = {prim::kPrimExpandDims};
    for (size_t i = 1; i < inputs.size(); ++i) {
      auto &input = inputs.at(i);
      if (!IsRef(input)) {
        continue;
      }
      // load ref users
      auto loads = FindLoadUsers(input);
      for (auto load : loads) {
        std::unordered_set<AnfNodePtr> load_users = FindUsers(load);
        std::unordered_set<AnfNodePtr> real_users;
        for (auto load_user : load_users) {
          // check the special operator, only one level of user is considered for now
          if (IsOneOfPrimitive(load_user, special_operators)) {
            std::unordered_set<AnfNodePtr> special_real_users = GetSpecialOperatorRealUsers(load_user);
            real_users.insert(special_real_users.begin(), special_real_users.end());
          } else {
            real_users.insert(load_user);
          }
        }
        AddInputEdges(last_input->cast<CNodePtr>(), real_users);
      }
    }
  }

  bool IsInUpdateState(const AnfNodePtr &load_user, const CNodePtr &update_state) {
    const size_t attach_index = 2;
    const size_t input_size = update_state->inputs().size();
    for (size_t index = attach_index; index < input_size; index++) {
      auto attach = update_state->input(attach_index);
      if (attach == load_user) {
        return true;
      }
      if (IsPrimitiveCNode(attach, prim::kPrimMakeTuple)) {
        auto attach_cnode = attach->cast<CNodePtr>();
        auto inputs = attach_cnode->inputs();
        bool has_load_user =
          std::any_of(inputs.begin() + 1, inputs.end(), [load_user](const auto &input) { return input == load_user; });
        if (has_load_user) {
          return true;
        }
      }
    }
    return false;
  }

  // Add load users as input edges of the update_state node.
  void AddInputEdges(const CNodePtr &update_state, const std::unordered_set<AnfNodePtr> &load_users) {
    auto sorted_load_users = SortLoadUsers(load_users);
    for (auto &load_user : sorted_load_users) {
      if (IsPrimitiveCNode(load_user, prim::kPrimMakeTuple) || IsPrimitiveCNode(load_user, prim::kPrimUpdateState)) {
        continue;
      }
      if (!IsDependOn(load_user, update_state)) {
        processed_nodes_.insert(load_user);
        if (!IsInUpdateState(load_user, update_state)) {
          manager_->AddEdge(update_state, load_user);
        }
      }
    }
  }

  // Sort load users by their topo sort order.
  std::vector<AnfNodePtr> SortLoadUsers(const std::unordered_set<AnfNodePtr> &load_users) {
    std::vector<AnfNodePtr> vec{load_users.begin(), load_users.end()};
    std::sort(vec.begin(), vec.end(), [this](const AnfNodePtr &a, const AnfNodePtr &b) { return IsBefore(a, b); });
    return vec;
  }

  // Check if the load user node depend on the given UpdateState node.
  bool IsDependOn(const AnfNodePtr &load_user, const AnfNodePtr &update_state) {
    size_t update_state_order = topo_sort_map_[update_state];
    if (topo_sort_map_[load_user] < update_state_order) {
      return false;
    }
    auto user_cnode = dyn_cast<CNode>(load_user);
    if (user_cnode == nullptr) {
      return false;
    }
    size_t seen = NewSeenGeneration();
    std::queue<CNodePtr> q;
    user_cnode->seen_ = seen;
    q.push(user_cnode);
    while (!q.empty()) {
      auto cnode = q.front();
      q.pop();
      for (auto &input : cnode->inputs()) {
        if (input == update_state) {
          // Dependency found.
          return true;
        }
        if (input->seen_ == seen) {
          // Skip visited nodes.
          continue;
        }
        if (topo_sort_map_[input] < update_state_order) {
          // Skip input nodes that before the UpdateState node.
          continue;
        }
        auto input_cnode = dyn_cast<CNode>(input);
        if (input_cnode != nullptr) {
          input_cnode->seen_ = seen;
          q.push(input_cnode);
        }
      }
    }
    return false;
  }

  bool IsBefore(const AnfNodePtr &node1, const AnfNodePtr &node2) {
    return topo_sort_map_[node1] < topo_sort_map_[node2];
  }

  // Find Load or parameter users as the candidate nodes to enforce order of execution.
  std::unordered_set<AnfNodePtr> FindUsers(const AnfNodePtr &load_or_param) {
    auto &node_users = manager_->node_users();
    auto iter = node_users.find(load_or_param);
    if (iter == node_users.end()) {
      return {};
    }
    std::unordered_set<AnfNodePtr> load_param_users;
    auto &users = iter->second;
    for (auto &user : users) {
      auto &user_node = user.first;
      if (processed_nodes_.find(user_node) != processed_nodes_.end()) {
        // Skip processed nodes.
        continue;
      }
      auto cnode = dyn_cast<CNode>(user_node);
      MS_EXCEPTION_IF_NULL(cnode);
      load_param_users.insert(cnode);
    }
    return load_param_users;
  }

  std::unordered_set<AnfNodePtr> FindLoadUsers(const AnfNodePtr &param) {
    auto &node_users = manager_->node_users();
    auto iter = node_users.find(param);
    if (iter == node_users.end()) {
      return {};
    }
    std::unordered_set<AnfNodePtr> loads;
    auto &users = iter->second;
    for (auto &user : users) {
      auto &user_node = user.first;
      if (IsPrimitiveCNode(user_node, prim::kPrimLoad)) {
        loads.insert(user_node);
      }
    }
    return loads;
  }

  const FuncGraphPtr &func_graph_;
  FuncGraphManagerPtr manager_;
  std::unordered_map<AnfNodePtr, size_t> topo_sort_map_;
  std::unordered_set<AnfNodePtr> processed_nodes_;
};

}  // namespace

// Enforce order of execution for Load users node.
void OrderEnforce(const FuncGraphPtr &func_graph) {
  OrderEnforcer enforcer(func_graph);
  enforcer.Run();
  auto fg_used_total = func_graph->func_graphs_used_total();
  for (auto &fg : fg_used_total) {
    OrderEnforcer fg_enforcer(fg);
    fg_enforcer.Run();
  }
}
}  // namespace mindspore::pipeline
