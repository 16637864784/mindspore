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

#include "ops/layer_norm.h"
#include "ops/op_utils.h"
#include "utils/check_convert_utils.h"

namespace mindspore {
namespace ops {
namespace {
ShapeVector CalLayerNormMeanAndVarShape(int64_t begin_norm_axis, const ShapeVector &input_shape) {
  auto mean_var_shape_value = input_shape;
  const size_t input_rank = input_shape.size();
  if (begin_norm_axis == -1) {
    mean_var_shape_value[input_rank - 1] = 1;
  } else {
    for (size_t i = begin_norm_axis; i < input_rank; i++) {
      mean_var_shape_value[i] = 1;
    }
  }
  return mean_var_shape_value;
}
}  // namespace

AbstractBasePtr LayerNormInfer(const abstract::AnalysisEnginePtr &, const PrimitivePtr &primitive,
                               const std::vector<AbstractBasePtr> &input_args) {
  // Inputs: three tensors(x, gamma, beta).
  // outputs: y, mean, variance
  MS_EXCEPTION_IF_NULL(primitive);
  const std::string op_name = primitive->name();
  CheckAndConvertUtils::CheckInteger("input numbers", input_args.size(), kEqual, 3, op_name);
  auto input_x = CheckAndConvertUtils::CheckArgs<abstract::AbstractTensor>(op_name, input_args, 0);
  auto gamma = CheckAndConvertUtils::CheckArgs<abstract::AbstractTensor>(op_name, input_args, 1);
  auto beta = CheckAndConvertUtils::CheckArgs<abstract::AbstractTensor>(op_name, input_args, 2);

  auto input_shape = input_x->shape();
  auto const &input_shape_list = input_shape->shape();
  const size_t input_rank = input_shape_list.size();
  if (input_rank == 0) {
    MS_LOG(EXCEPTION) << "input_rank should not be zero";
  }

  // begin_norm_axis and begin_params_axis should be smaller than the size of input_x and >= -1
  ValuePtr bna_ptr = primitive->GetAttr("begin_norm_axis");
  int64_t begin_norm_axis = abstract::CheckAxis(op_name, bna_ptr, -1, SizeToLong(input_rank) - 1);

  ValuePtr bpa_ptr = primitive->GetAttr("begin_params_axis");
  int64_t begin_params_axis = abstract::CheckAxis(op_name, bpa_ptr, -1, SizeToLong(input_rank) - 1);
  begin_params_axis = abstract::GetPositiveAxis(begin_params_axis, input_rank);

  // the beta and gama shape should be x_shape[begin_params_axis:]
  auto valid_types = {kFloat16, kFloat32};
  (void)CheckAndConvertUtils::CheckTensorTypeValid("x_dtype", input_args[0]->BuildType(), valid_types, op_name);
  (void)CheckAndConvertUtils::CheckTensorTypeValid("gamma_dtype", input_args[1]->BuildType(), valid_types, op_name);
  (void)CheckAndConvertUtils::CheckTensorTypeValid("beta_dtype", input_args[2]->BuildType(), valid_types, op_name);

  auto gamma_shape = dyn_cast<abstract::Shape>(gamma->BuildShape());
  auto beta_shape = dyn_cast<abstract::Shape>(beta->BuildShape());
  MS_EXCEPTION_IF_NULL(gamma_shape);
  MS_EXCEPTION_IF_NULL(beta_shape);

  auto const &gamma_shape_list = gamma_shape->shape();
  auto const &beta_shape_list = beta_shape->shape();
  if (gamma_shape_list.empty() || beta_shape_list.empty()) {
    MS_LOG(EXCEPTION) << "LayerNorm evaluator gamma or beta is a AbstractScalar that is not support.";
  }

  size_t begin_params_axis_u = LongToSize(begin_params_axis);
  if ((begin_params_axis_u > input_shape_list.size()) ||
      (gamma_shape_list.size() + begin_params_axis_u < input_shape_list.size()) ||
      (beta_shape_list.size() + begin_params_axis_u < input_shape_list.size())) {
    MS_LOG(EXCEPTION) << "Gamma and beta shape get wrong size.";
  }
  for (size_t i = begin_params_axis_u; i < input_shape_list.size(); ++i) {
    size_t gamma_beta_shape_dim = i - begin_params_axis_u;
    if ((gamma_shape_list[gamma_beta_shape_dim] != input_shape_list[i]) ||
        (beta_shape_list[gamma_beta_shape_dim] != input_shape_list[i])) {
      MS_LOG(EXCEPTION) << "Gamma or beta shape not match input shape, input_shape=" << input_shape->ToString()
                        << ", gamma_shape=" << gamma_shape->ToString() << ", beta_shape=" << beta_shape->ToString();
    }
  }

  std::vector<BaseShapePtr> shapes_list = {input_x->BuildShape()};
  std::vector<TypePtr> types_list = {input_x->BuildType(), input_x->BuildType(), input_x->BuildType()};
  auto mean_var_shape = CalLayerNormMeanAndVarShape(begin_norm_axis, input_shape->shape());
  auto input_min_shape = input_shape->min_shape();
  auto input_max_shape = input_shape->max_shape();
  if (input_min_shape.empty() || input_max_shape.empty()) {
    shapes_list.emplace_back(std::make_shared<abstract::Shape>(mean_var_shape));
    shapes_list.emplace_back(std::make_shared<abstract::Shape>(mean_var_shape));
  } else {
    auto mean_var_shape_min = CalLayerNormMeanAndVarShape(begin_norm_axis, input_min_shape);
    auto mean_var_shape_max = CalLayerNormMeanAndVarShape(begin_norm_axis, input_min_shape);
    shapes_list.emplace_back(std::make_shared<abstract::Shape>(mean_var_shape, mean_var_shape_min, mean_var_shape_max));
    shapes_list.emplace_back(std::make_shared<abstract::Shape>(mean_var_shape, mean_var_shape_min, mean_var_shape_max));
  }
  return abstract::MakeAbstract(std::make_shared<abstract::TupleShape>(shapes_list),
                                std::make_shared<Tuple>(types_list));
}

void LayerNorm::Init(const int64_t begin_norm_axis, const int64_t begin_params_axis, const float epsilon) {
  this->set_begin_norm_axis(begin_norm_axis);
  this->set_begin_params_axis(begin_params_axis);
  this->set_epsilon(epsilon);
}
void LayerNorm::set_begin_norm_axis(const int64_t begin_norm_axis) {
  this->AddAttr(kBeginNormAxis, MakeValue(begin_norm_axis));
}
void LayerNorm::set_begin_params_axis(const int64_t begin_params_axis) {
  this->AddAttr(kBeginParamsAxis, MakeValue(begin_params_axis));
}
void LayerNorm::set_epsilon(const float epsilon) { this->AddAttr(kEpsilon, MakeValue(epsilon)); }

int64_t LayerNorm::get_begin_norm_axis() const {
  auto value_ptr = this->GetAttr(kBeginNormAxis);
  return GetValue<int64_t>(value_ptr);
}
int64_t LayerNorm::get_begin_params_axis() const {
  auto value_ptr = this->GetAttr(kBeginParamsAxis);
  return GetValue<int64_t>(value_ptr);
}
float LayerNorm::get_epsilon() const {
  auto value_ptr = this->GetAttr(kEpsilon);
  return GetValue<float>(value_ptr);
}
REGISTER_PRIMITIVE_EVAL_IMPL(LayerNorm, prim::kPrimLayerNorm, LayerNormInfer, nullptr, true);
}  // namespace ops
}  // namespace mindspore
