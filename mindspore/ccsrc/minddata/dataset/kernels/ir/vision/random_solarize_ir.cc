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

#include "minddata/dataset/kernels/ir/vision/random_solarize_ir.h"

#ifndef ENABLE_ANDROID
#include "minddata/dataset/kernels/image/random_solarize_op.h"
#endif

#include "minddata/dataset/kernels/ir/validators.h"

namespace mindspore {
namespace dataset {

namespace vision {

#ifndef ENABLE_ANDROID

// RandomSolarizeOperation.
RandomSolarizeOperation::RandomSolarizeOperation(std::vector<uint8_t> threshold)
    : TensorOperation(true), threshold_(threshold) {}

RandomSolarizeOperation::~RandomSolarizeOperation() = default;

std::string RandomSolarizeOperation::Name() const { return kRandomSolarizeOperation; }

Status RandomSolarizeOperation::ValidateParams() {
  if (threshold_.size() != 2) {
    std::string err_msg =
      "RandomSolarize: threshold must be a vector of two values, got: " + std::to_string(threshold_.size());
    MS_LOG(ERROR) << err_msg;
    RETURN_STATUS_SYNTAX_ERROR(err_msg);
  }
  for (int32_t i = 0; i < threshold_.size(); ++i) {
    if (threshold_[i] < 0 || threshold_[i] > 255) {
      std::string err_msg =
        "RandomSolarize: threshold has to be between 0 and 255, got:" + std::to_string(threshold_[i]);
      MS_LOG(ERROR) << err_msg;
      RETURN_STATUS_SYNTAX_ERROR(err_msg);
    }
  }
  if (threshold_[0] > threshold_[1]) {
    std::string err_msg = "RandomSolarize: threshold must be passed in a (min, max) format";
    MS_LOG(ERROR) << err_msg;
    RETURN_STATUS_SYNTAX_ERROR(err_msg);
  }
  return Status::OK();
}

std::shared_ptr<TensorOp> RandomSolarizeOperation::Build() {
  std::shared_ptr<RandomSolarizeOp> tensor_op = std::make_shared<RandomSolarizeOp>(threshold_);
  return tensor_op;
}

Status RandomSolarizeOperation::to_json(nlohmann::json *out_json) {
  (*out_json)["threshold"] = threshold_;
  return Status::OK();
}

#endif

}  // namespace vision
}  // namespace dataset
}  // namespace mindspore
