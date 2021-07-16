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
#include <algorithm>

#include "minddata/dataset/kernels/ir/vision/random_affine_ir.h"

#include "minddata/dataset/kernels/image/random_affine_op.h"

#include "minddata/dataset/kernels/ir/validators.h"

namespace mindspore {
namespace dataset {
namespace vision {

constexpr size_t dimension_zero = 0;
constexpr size_t dimension_one = 1;
constexpr size_t dimension_two = 2;
constexpr size_t dimension_three = 3;
constexpr size_t size_two = 2;
constexpr size_t size_three = 3;
constexpr size_t size_four = 4;

// RandomAffineOperation
RandomAffineOperation::RandomAffineOperation(const std::vector<float_t> &degrees,
                                             const std::vector<float_t> &translate_range,
                                             const std::vector<float_t> &scale_range,
                                             const std::vector<float_t> &shear_ranges, InterpolationMode interpolation,
                                             const std::vector<uint8_t> &fill_value)
    : degrees_(degrees),
      translate_range_(translate_range),
      scale_range_(scale_range),
      shear_ranges_(shear_ranges),
      interpolation_(interpolation),
      fill_value_(fill_value) {
  random_op_ = true;
}

RandomAffineOperation::~RandomAffineOperation() = default;

std::string RandomAffineOperation::Name() const { return kRandomAffineOperation; }

Status RandomAffineOperation::ValidateParams() {
  // Degrees
  if (degrees_.size() != size_two) {
    std::string err_msg =
      "RandomAffine: degrees expecting size 2, got: degrees.size() = " + std::to_string(degrees_.size());
    MS_LOG(ERROR) << err_msg;
    RETURN_STATUS_SYNTAX_ERROR(err_msg);
  }
  if (degrees_[dimension_zero] > degrees_[dimension_one]) {
    std::string err_msg = "RandomAffine: minimum of degrees range is greater than maximum: min = " +
                          std::to_string(degrees_[dimension_zero]) +
                          ", max = " + std::to_string(degrees_[dimension_one]);
    MS_LOG(ERROR) << err_msg;
    RETURN_STATUS_SYNTAX_ERROR(err_msg);
  }
  // Translate
  if (translate_range_.size() != size_two && translate_range_.size() != size_four) {
    std::string err_msg = "RandomAffine: translate_range expecting size 2 or 4, got: translate_range.size() = " +
                          std::to_string(translate_range_.size());
    MS_LOG(ERROR) << err_msg;
    RETURN_STATUS_SYNTAX_ERROR(err_msg);
  }
  if (translate_range_[dimension_zero] > translate_range_[dimension_one]) {
    std::string err_msg = "RandomAffine: minimum of translate range on x is greater than maximum: min = " +
                          std::to_string(translate_range_[dimension_zero]) +
                          ", max = " + std::to_string(translate_range_[dimension_one]);
    MS_LOG(ERROR) << err_msg;
    RETURN_STATUS_SYNTAX_ERROR(err_msg);
  }
  RETURN_IF_NOT_OK(
    ValidateScalar("RandomAffine", "translate", translate_range_[dimension_zero], {-1, 1}, false, false));
  RETURN_IF_NOT_OK(ValidateScalar("RandomAffine", "translate", translate_range_[dimension_one], {-1, 1}, false, false));
  if (translate_range_.size() == size_four) {
    if (translate_range_[dimension_two] > translate_range_[dimension_three]) {
      std::string err_msg = "RandomAffine: minimum of translate range on y is greater than maximum: min = " +
                            std::to_string(translate_range_[dimension_two]) +
                            ", max = " + std::to_string(translate_range_[dimension_three]);
      MS_LOG(ERROR) << err_msg;
      RETURN_STATUS_SYNTAX_ERROR(err_msg);
    }
    RETURN_IF_NOT_OK(
      ValidateScalar("RandomAffine", "translate", translate_range_[dimension_two], {-1, 1}, false, false));
    RETURN_IF_NOT_OK(
      ValidateScalar("RandomAffine", "translate", translate_range_[dimension_three], {-1, 1}, false, false));
  }
  // Scale
  RETURN_IF_NOT_OK(ValidateVectorScale("RandomAffine", scale_range_));
  // Shear
  if (shear_ranges_.size() != size_two && shear_ranges_.size() != size_four) {
    std::string err_msg = "RandomAffine: shear_ranges expecting size 2 or 4, got: shear_ranges.size() = " +
                          std::to_string(shear_ranges_.size());
    MS_LOG(ERROR) << err_msg;
    RETURN_STATUS_SYNTAX_ERROR(err_msg);
  }
  if (shear_ranges_[dimension_zero] > shear_ranges_[dimension_one]) {
    std::string err_msg = "RandomAffine: minimum of horizontal shear range is greater than maximum: min = " +
                          std::to_string(shear_ranges_[dimension_zero]) +
                          ", max = " + std::to_string(shear_ranges_[dimension_one]);
    MS_LOG(ERROR) << err_msg;
    RETURN_STATUS_SYNTAX_ERROR(err_msg);
  }
  if (shear_ranges_.size() == size_four && shear_ranges_[dimension_two] > shear_ranges_[dimension_three]) {
    std::string err_msg = "RandomAffine: minimum of vertical shear range is greater than maximum: min = " +
                          std::to_string(shear_ranges_[dimension_two]) +
                          ", max = " + std::to_string(scale_range_[dimension_three]);
    MS_LOG(ERROR) << err_msg;
    RETURN_STATUS_SYNTAX_ERROR(err_msg);
  }
  // Fill Value
  RETURN_IF_NOT_OK(ValidateVectorFillvalue("RandomAffine", fill_value_));
  return Status::OK();
}

std::shared_ptr<TensorOp> RandomAffineOperation::Build() {
  if (shear_ranges_.size() == size_two) {
    shear_ranges_.resize(size_four);
  }
  if (translate_range_.size() == size_two) {
    translate_range_.resize(size_four);
  }
  std::vector<uint8_t> fill_value = {fill_value_[dimension_zero], fill_value_[dimension_zero],
                                     fill_value_[dimension_zero]};
  if (fill_value_.size() == size_three) {
    fill_value[dimension_one] = fill_value_[dimension_one];
    fill_value[dimension_two] = fill_value_[dimension_two];
  }

  auto tensor_op = std::make_shared<RandomAffineOp>(degrees_, translate_range_, scale_range_, shear_ranges_,
                                                    interpolation_, fill_value);
  return tensor_op;
}

Status RandomAffineOperation::to_json(nlohmann::json *out_json) {
  nlohmann::json args;
  args["degrees"] = degrees_;
  args["translate"] = translate_range_;
  args["scale"] = scale_range_;
  args["shear"] = shear_ranges_;
  args["resample"] = interpolation_;
  args["fill_value"] = fill_value_;
  *out_json = args;
  return Status::OK();
}
}  // namespace vision
}  // namespace dataset
}  // namespace mindspore
