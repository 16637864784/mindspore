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

#include "minddata/dataset/kernels/ir/vision/normalize_pad_ir.h"

#ifndef ENABLE_ANDROID
#include "minddata/dataset/kernels/image/normalize_pad_op.h"
#endif

#include "minddata/dataset/kernels/ir/validators.h"

namespace mindspore {
namespace dataset {

namespace vision {

#ifndef ENABLE_ANDROID
// NormalizePadOperation
NormalizePadOperation::NormalizePadOperation(const std::vector<float> &mean, const std::vector<float> &std,
                                             const std::string &dtype)
    : mean_(mean), std_(std), dtype_(dtype) {}

NormalizePadOperation::~NormalizePadOperation() = default;

std::string NormalizePadOperation::Name() const { return kNormalizePadOperation; }

Status NormalizePadOperation::ValidateParams() {
  RETURN_IF_NOT_OK(ValidateVectorMeanStd("NormalizePad", mean_, std_));
  if (dtype_ != "float32" && dtype_ != "float16") {
    std::string err_msg = "NormalizePad: dtype must be float32 or float16, but got: " + dtype_;
    MS_LOG(ERROR) << err_msg;
    RETURN_STATUS_SYNTAX_ERROR(err_msg);
  }
  return Status::OK();
}

std::shared_ptr<TensorOp> NormalizePadOperation::Build() {
  return std::make_shared<NormalizePadOp>(mean_[0], mean_[1], mean_[2], std_[0], std_[1], std_[2], dtype_);
}

Status NormalizePadOperation::to_json(nlohmann::json *out_json) {
  nlohmann::json args;
  args["mean"] = mean_;
  args["std"] = std_;
  args["dtype"] = dtype_;
  *out_json = args;
  return Status::OK();
}
#endif

}  // namespace vision
}  // namespace dataset
}  // namespace mindspore
