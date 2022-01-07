/**
 * Copyright 2020-2022 Huawei Technologies Co., Ltd
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

#ifndef MINDSPORE_CCSRC_FRONTEND_OPTIMIZER_IRPASS_CALL_GRAPH_TRANSFORM_H_
#define MINDSPORE_CCSRC_FRONTEND_OPTIMIZER_IRPASS_CALL_GRAPH_TRANSFORM_H_

#include <algorithm>
#include <memory>
#include <vector>

#include "utils/hash_map.h"
#include "utils/hash_set.h"
#include "ir/func_graph.h"
#include "ir/func_graph_cloner.h"
#include "frontend/optimizer/optimizer_caller.h"
#include "frontend/optimizer/anf_visitor.h"
#include "frontend/operator/ops.h"
#include "frontend/optimizer/irpass.h"
#include "frontend/optimizer/optimizer.h"
#include "frontend/optimizer/graph_transform.h"

namespace mindspore {
namespace opt {
namespace irpass {
constexpr int kInputZero = 0;
constexpr int kInputOne = 1;
constexpr int kInputTwo = 2;
constexpr int kInputThree = 3;
// {G, Xs}-->transform graph call tuple inputs to flat inputs.
class GraphCallTupleTransform : public AnfVisitor {
 public:
  explicit GraphCallTupleTransform(GraphTupleParamTransform &transformer) : graph_transform_(transformer) {}
  ~GraphCallTupleTransform() override = default;
  AnfNodePtr operator()(const OptimizerPtr &optimizer, const AnfNodePtr &node) override {
    if (!node->isa<CNode>() || node->func_graph() == nullptr) {
      return nullptr;
    }

    auto cnode = node->cast<CNodePtr>();
    MS_EXCEPTION_IF_NULL(cnode);
    auto &inputs = cnode->inputs();
    auto fg = GetValueNode<FuncGraphPtr>(inputs[kInputZero]);
    if (fg == nullptr) {
      return nullptr;
    }
    if (!CNodeHasTupleInput(cnode)) {
      return nullptr;
    }
    FuncGraphPtr transformed_fg = graph_transform_(fg, optimizer->manager());
    auto new_node = TransformCallGraph(transformed_fg, cnode);
    return new_node;
  }

 private:
  GraphTupleParamTransform &graph_transform_;
};

// {{switch, cond, true_branch, false_branch}, Xs} -->transform switch graph call tuple inputs to flat inputs.
class SwitchCallTupleTransform : public AnfVisitor {
 public:
  explicit SwitchCallTupleTransform(GraphTupleParamTransform &transformer) : graph_transform_(transformer) {}
  ~SwitchCallTupleTransform() override = default;
  AnfNodePtr operator()(const OptimizerPtr &optimizer, const AnfNodePtr &node) override {
    if (!node->isa<CNode>() || node->func_graph() == nullptr) {
      return nullptr;
    }
    auto switch_call_cnode = node->cast<CNodePtr>();
    auto call_inputs = switch_call_cnode->inputs();
    if (call_inputs.size() < 1) {
      return nullptr;
    }
    if (!IsPrimitiveCNode(call_inputs[kInputZero], prim::kPrimSwitch)) {
      return nullptr;
    }
    auto switch_cnode = call_inputs[kInputZero]->cast<CNodePtr>();
    auto switch_inputs = switch_cnode->inputs();
    if (switch_inputs.size() != 4) {
      return nullptr;
    }

    AnfNodePtr transformed = nullptr;
    bool true_br_changed = TransformBranchNode(switch_inputs[kInputTwo], optimizer->manager(), &transformed);
    if (true_br_changed) {
      switch_inputs[kInputTwo] = transformed;
    }
    bool false_br_changed = TransformBranchNode(switch_inputs[kInputThree], optimizer->manager(), &transformed);
    if (false_br_changed) {
      switch_inputs[kInputThree] = transformed;
    }
    if (true_br_changed || false_br_changed) {
      call_inputs[kInputZero] = switch_cnode->func_graph()->NewCNode(switch_inputs);
    }
    if (CNodeHasTupleInput(switch_call_cnode)) {
      return TransformSwitchCall(call_inputs[kInputZero], switch_call_cnode);
    }
    if (true_br_changed || false_br_changed) {
      return switch_call_cnode->func_graph()->NewCNode(call_inputs);
    }
    return nullptr;
  }

  bool TransformBranchNode(const AnfNodePtr &node, FuncGraphManagerPtr mng, AnfNodePtr *trans_node) {
    if (IsValueNode<FuncGraph>(node)) {
      FuncGraphPtr fg = GetValueNode<FuncGraphPtr>(node);
      if (FuncGraphHasTupleInput(fg)) {
        FuncGraphPtr transformed_fg = graph_transform_(fg, mng);
        *trans_node = NewValueNode(transformed_fg);
        return true;
      }
      return false;
    }
    if (IsPrimitiveCNode(node, prim::kPrimPartial)) {
      auto partial_inputs = node->cast<CNodePtr>()->inputs();
      if (IsValueNode<FuncGraph>(partial_inputs[kInputOne])) {
        FuncGraphPtr fg = GetValueNode<FuncGraphPtr>(partial_inputs[kInputOne]);
        if (FuncGraphHasTupleInput(fg)) {
          fg = graph_transform_(fg, mng);
        }
        if (CNodeHasTupleInput(node->cast<CNodePtr>())) {
          *trans_node = TransformPartial(fg, node->cast<CNodePtr>());
          return true;
        }
      }
      return false;
    }

    MS_LOG(WARNING) << "Got unexpected switch branch node " << node->DebugString();
    return false;
  }

 private:
  GraphTupleParamTransform &graph_transform_;
};

// {{switch_layer, index, {make_tuple, br1, br2,...,}}, Xs} ->
// transform switch layer graph call tuple inputs to flat inputs.
class SwitchLayerCallTupleTransform : public AnfVisitor {
 public:
  explicit SwitchLayerCallTupleTransform(GraphTupleParamTransform &transformer) : graph_transform_(transformer) {}
  ~SwitchLayerCallTupleTransform() override = default;
  AnfNodePtr operator()(const OptimizerPtr &optimizer, const AnfNodePtr &node) override {
    if (!node->isa<CNode>() || node->func_graph() == nullptr) {
      return nullptr;
    }
    auto switch_layer_call_cnode = node->cast<CNodePtr>();
    auto call_inputs = switch_layer_call_cnode->inputs();
    if (call_inputs.size() < 1) {
      return nullptr;
    }
    if (!IsPrimitiveCNode(call_inputs[kInputZero], prim::kPrimSwitchLayer)) {
      return nullptr;
    }
    auto switch_layer_cnode = call_inputs[kInputZero]->cast<CNodePtr>();
    auto switch_layer_inputs = switch_layer_cnode->inputs();
    if (switch_layer_inputs.size() != 3) {
      return nullptr;
    }

    AnfNodePtr transformed = nullptr;
    bool layer_changed = TransformLayerNode(switch_layer_inputs[kInputTwo], optimizer->manager(), &transformed);
    if (layer_changed) {
      transformed->set_abstract(switch_layer_inputs[kInputTwo]->abstract());
      switch_layer_inputs[kInputTwo] = transformed;
      auto new_switch_layer = switch_layer_call_cnode->func_graph()->NewCNode(switch_layer_inputs);
      new_switch_layer->set_abstract(switch_layer_cnode->abstract());
      call_inputs[kInputZero] = new_switch_layer;
    }
    if (CNodeHasTupleInput(switch_layer_call_cnode)) {
      return TransformSwitchCall(call_inputs[kInputZero], switch_layer_call_cnode);
    }
    if (layer_changed) {
      return switch_layer_call_cnode->func_graph()->NewCNode(call_inputs);
    }
    return nullptr;
  }

  bool TransformLayerNode(const AnfNodePtr &node, FuncGraphManagerPtr mng, AnfNodePtr *trans_node) {
    if (!IsPrimitiveCNode(node, prim::kPrimMakeTuple)) {
      MS_LOG(WARNING) << "SwitchLayer input is not MakeTuple";
      return false;
    }
    auto tuple_inputs = node->cast<CNodePtr>()->inputs();
    bool changed = false;
    for (size_t i = 1; i < tuple_inputs.size(); i++) {
      if (!IsValueNode<FuncGraph>(tuple_inputs[i])) {
        MS_LOG(WARNING) << "SwitchLayer input is not FuncGraph";
        return false;
      }
      FuncGraphPtr fg = GetValueNode<FuncGraphPtr>(tuple_inputs[i]);
      if (FuncGraphHasTupleInput(fg)) {
        FuncGraphPtr transformed_fg = graph_transform_(fg, mng);
        auto new_value_node = NewValueNode(transformed_fg);
        new_value_node->set_abstract(tuple_inputs[i]->abstract());
        tuple_inputs[i] = new_value_node;
        changed = true;
      }
    }
    if (changed) {
      *trans_node = node->func_graph()->NewCNode(tuple_inputs);
    }
    return changed;
  }

 private:
  GraphTupleParamTransform &graph_transform_;
};

class CallGraphTupleTransform : public OptimizerCaller {
 public:
  CallGraphTupleTransform()
      : graph_transformer_(),
        graph_call_transform_(std::make_shared<GraphCallTupleTransform>(graph_transformer_)),
        switch_call_transform_(std::make_shared<SwitchCallTupleTransform>(graph_transformer_)),
        switch_layer_call_transform_(std::make_shared<SwitchLayerCallTupleTransform>(graph_transformer_)) {
    transformers_.emplace_back(graph_call_transform_);
    transformers_.emplace_back(switch_call_transform_);
    transformers_.emplace_back(switch_layer_call_transform_);
  }
  ~CallGraphTupleTransform() = default;

  AnfNodePtr operator()(const OptimizerPtr &optimizer, const AnfNodePtr &node) override {
    AnfNodePtr new_node;
    for (auto &transform : transformers_) {
      new_node = (*transform)(optimizer, node);
      if (new_node != nullptr) {
        return new_node;
      }
    }
    return nullptr;
  }

 private:
  GraphTupleParamTransform graph_transformer_;
  OptimizerCallerPtr graph_call_transform_;
  OptimizerCallerPtr switch_call_transform_;
  OptimizerCallerPtr switch_layer_call_transform_;
  std::vector<OptimizerCallerPtr> transformers_{};
};
}  // namespace irpass
}  // namespace opt
}  // namespace mindspore
#endif  // MINDSPORE_CCSRC_FRONTEND_OPTIMIZER_IRPASS_CALL_GRAPH_TRANSFORM_H_
