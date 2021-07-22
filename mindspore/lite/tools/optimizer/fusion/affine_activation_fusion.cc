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

#include "tools/optimizer/fusion/affine_activation_fusion.h"
#include <memory>
#include "tools/optimizer/common/gllo_utils.h"
#include "ops/fusion/activation.h"
#include "ops/affine.h"

namespace mindspore::opt {
static bool IsAffineNode(const BaseRef &n) {
  if (utils::isa<AnfNodePtr>(n)) {
    auto anf_node = utils::cast<AnfNodePtr>(n);
    return CheckPrimitiveType(anf_node, prim::kPrimAffine);
  }
  return false;
}

const BaseRef AffineActivationFusion::DefinePattern() const {
  auto activation_var = std::make_shared<CondVar>(IsActivationNode);
  auto affine_var = std::make_shared<CondVar>(IsAffineNode);
  return VectorRef({activation_var, affine_var});
}

const AnfNodePtr AffineActivationFusion::Process(const FuncGraphPtr &func_graph, const AnfNodePtr &node,
                                                 const EquivPtr &equiv) const {
  MS_ASSERT(equiv != nullptr);
  if (CheckIfFuncGraphIsNull(func_graph) != lite::RET_OK || CheckIfAnfNodeIsNull(node) != lite::RET_OK) {
    lite::ReturnCode::GetSingleReturnCode()->UpdateReturnCode(lite::RET_NULL_PTR);
    return nullptr;
  }
  // activation
  if (!CheckPrimitiveType(node, prim::kPrimActivation)) {
    MS_LOG(ERROR) << "the layer processed by affine fusion is not matmul.";
    lite::ReturnCode::GetSingleReturnCode()->UpdateReturnCode(lite::RET_PARAM_INVALID);
    return nullptr;
  }
  auto activation_node = node->cast<CNodePtr>();
  if (CheckIfCNodeIsNull(activation_node) != lite::RET_OK) {
    MS_LOG(ERROR) << "the matmul_node is null.";
    lite::ReturnCode::GetSingleReturnCode()->UpdateReturnCode(lite::RET_NULL_PTR);
    return nullptr;
  }
  auto activation_prim = GetValueNode<std::shared_ptr<ops::Activation>>(activation_node->input(kAnfPrimitiveIndex));
  MS_ASSERT(activation_prim != nullptr);
  AnfNodePtr pre_node = activation_node->input(1);
  if (!CheckPrimitiveType(pre_node, prim::kPrimAffine)) {
    MS_LOG(ERROR) << "previous node is not splice.";
    lite::ReturnCode::GetSingleReturnCode()->UpdateReturnCode(lite::RET_PARAM_INVALID);
    return nullptr;
  }
  auto affine_node = pre_node->cast<CNodePtr>();
  if (CheckIfAnfNodeIsNull(pre_node) != lite::RET_OK) {
    MS_LOG(ERROR) << "the splice_node is null.";
    lite::ReturnCode::GetSingleReturnCode()->UpdateReturnCode(lite::RET_NULL_PTR);
    return nullptr;
  }
  auto affine_prim = GetValueNode<std::shared_ptr<ops::Affine>>(affine_node->input(kAnfPrimitiveIndex));
  MS_ASSERT(prim != nullptr);

  if (!activation_prim->HasAttr(ops::kActivationType)) {
    MS_LOG(ERROR) << "the kActivationType is null.";
    lite::ReturnCode::GetSingleReturnCode()->UpdateReturnCode(lite::RET_NULL_PTR);
    return nullptr;
  }
  affine_prim->set_activation_type(activation_prim->get_activation_type());

  return affine_node;
}
}  // namespace mindspore::opt
