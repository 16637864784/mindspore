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

#include "tools/optimizer/fisson/node_out_shapes.h"
#include <vector>
#include <string>
#include "tools/optimizer/fisson/fisson_util.h"

namespace mindspore {
namespace opt {

AnfNodePtr NodeOutShapes::Run(const FuncGraphPtr &func_graph, const AnfNodePtr &node) {
  if (CheckIfFuncGraphIsNull(func_graph) != lite::RET_OK || CheckIfAnfNodeIsNull(node) != lite::RET_OK) {
    return nullptr;
  }
  std::vector<ShapeVector> input_shapes;
  std::vector<ShapeVector> output_shapes;
  if (!utils::isa<CNodePtr>(node)) {
    return nullptr;
  }
  auto cnode = node->cast<CNodePtr>();
  // input
  for (auto input_node : cnode->inputs()) {
    if (utils::isa<CNodePtr>(input_node) || utils::isa<ParameterPtr>(input_node)) {
      auto in_shape = input_node->Shape();
      if (in_shape == nullptr) {
        MS_LOG(ERROR) << "The shape is null.";
        lite::ReturnCode::GetSingleReturnCode()->UpdateReturnCode(lite::RET_NULL_PTR);
        return nullptr;
      }
      if (utils::isa<abstract::ShapePtr>(in_shape)) {
        const auto &shape = in_shape->cast<abstract::ShapePtr>()->shape();
        input_shapes.push_back(shape);
      } else {
        MS_LOG(ERROR) << "currently not support tuple";
      }
    }
  }
  // output
  auto out_shape = cnode->Shape();
  if (out_shape == nullptr) {
    MS_LOG(ERROR) << "The shape is null.";
    lite::ReturnCode::GetSingleReturnCode()->UpdateReturnCode(lite::RET_NULL_PTR);
    return nullptr;
  }
  if (utils::isa<abstract::TupleShapePtr>(out_shape)) {
    auto shape = out_shape->cast<abstract::TupleShapePtr>();
    for (size_t i = 0; i < shape->size(); ++i) {
      const auto &shape_ptr = (*shape)[i];
      if (!utils::isa<abstract::ShapePtr>(shape_ptr)) {
        MS_LOG(ERROR) << "shape_ptr is not ShapePtr.";
        lite::ReturnCode::GetSingleReturnCode()->UpdateReturnCode(lite::RET_NULL_PTR);
        return nullptr;
      }
      output_shapes.push_back(shape_ptr->cast<abstract::ShapePtr>()->shape());
    }
  } else if (utils::isa<abstract::ShapePtr>(out_shape)) {
    const auto &shape = out_shape->cast<abstract::ShapePtr>()->shape();
    output_shapes.push_back(shape);
  }

  std::string name = cnode->fullname_with_scope();
  g_graph_nodes_out_shapes[name].push_back(input_shapes);
  g_graph_nodes_out_shapes[name].push_back(output_shapes);
  return nullptr;
}
}  // namespace opt
}  // namespace mindspore
