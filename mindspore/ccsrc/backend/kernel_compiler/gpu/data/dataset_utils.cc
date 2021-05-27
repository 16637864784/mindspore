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

#include "backend/kernel_compiler/gpu/data/dataset_utils.h"
#include <algorithm>
#include "backend/session/anf_runtime_algorithm.h"

namespace mindspore {
namespace kernel {
size_t UnitSizeInBytes(const mindspore::TypeId &t) {
  size_t bytes = 0;
  switch (t) {
    case kNumberTypeBool:
    case kNumberTypeInt8:
    case kNumberTypeUInt8:
      bytes = 1;
      break;
    case kNumberTypeInt16:
    case kNumberTypeUInt16:
    case kNumberTypeFloat16:
      bytes = 2;
      break;
    case kNumberTypeInt:
    case kNumberTypeUInt:
    case kNumberTypeInt32:
    case kNumberTypeUInt32:
    case kNumberTypeFloat:
    case kNumberTypeFloat32:
      bytes = 4;
      break;
    case kNumberTypeUInt64:
    case kNumberTypeInt64:
    case kNumberTypeFloat64:
      bytes = 8;
      break;
    default:
      MS_LOG(EXCEPTION) << "Invalid types " << t;
      break;
  }

  return bytes;
}

int ElementNums(const std::vector<int> &shape) {
  if (shape.size() == 0) {
    return 0;
  }

  int nums = 1;
  for (size_t i = 0; i < shape.size(); i++) {
    nums *= shape[i];
  }

  return nums;
}

void GetShapeAndType(const CNodePtr &kernel_node, std::vector<std::vector<int>> *shapes, std::vector<TypePtr> *types) {
  MS_EXCEPTION_IF_NULL(shapes);
  MS_EXCEPTION_IF_NULL(types);
  std::vector<std::vector<int64_t>> shapes_me =
    AnfAlgo::GetNodeAttr<std::vector<std::vector<int64_t>>>(kernel_node, "shapes");
  (void)std::transform(shapes_me.begin(), shapes_me.end(), std::back_inserter(*shapes),
                       [](const std::vector<int64_t> &values) {
                         std::vector<int> shape;
                         (void)std::transform(values.begin(), values.end(), std::back_inserter(shape),
                                              [](const int64_t &value) { return static_cast<int>(value); });
                         return shape;
                       });

  *types = AnfAlgo::GetNodeAttr<std::vector<TypePtr>>(kernel_node, "types");
  if (shapes->size() != types->size()) {
    MS_LOG(EXCEPTION) << "Invalid shapes: " << *shapes << ", types: " << *types;
  }
}
}  // namespace kernel
}  // namespace mindspore
