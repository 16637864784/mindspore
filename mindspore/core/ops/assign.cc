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

#include <set>
#include <map>
#include <vector>
#include <memory>
#include <string>

#include "ops/assign.h"
#include "ops/op_utils.h"
#include "ir/dtype/ref.h"

namespace mindspore {
namespace ops {
AbstractBasePtr InferImplAssign(const abstract::AnalysisEnginePtr &, const PrimitivePtr &primitive,
                                const AbstractBasePtrList &args_spec_list) {
  MS_EXCEPTION_IF_NULL(primitive);
  auto prim_name = primitive->name();
  (void)CheckAndConvertUtils::CheckInteger("Assign infer", (CheckAndConvertUtils::GetRemoveMonadAbsNum(args_spec_list)),
                                           kEqual, 2, prim_name);
  auto check_types = common_valid_types;
  check_types.emplace(kBool);
  auto variable_type = args_spec_list[0]->BuildType();
  auto value_type = args_spec_list[1]->BuildType();
  CheckAndConvertUtils::CheckScalarOrTensorTypesSame(std::map<std::string, TypePtr>{{"value", value_type}}, check_types,
                                                     prim_name);
  if (variable_type->isa<RefKeyType>()) {
    return args_spec_list[1]->Broaden();
  }
  CheckAndConvertUtils::CheckTensorTypeValid("variable", variable_type, check_types, prim_name);
  return args_spec_list[0];
}
REGISTER_PRIMITIVE_EVAL_IMPL(Assign, prim::kPrimAssign, InferImplAssign, nullptr, true);
}  // namespace ops
}  // namespace mindspore
