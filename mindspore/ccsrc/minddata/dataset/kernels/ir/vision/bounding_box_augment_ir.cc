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

#include "minddata/dataset/kernels/ir/vision/bounding_box_augment_ir.h"

#ifndef ENABLE_ANDROID
#include "minddata/dataset/kernels/image/bounding_box_augment_op.h"
#endif

#include "minddata/dataset/kernels/ir/validators.h"

namespace mindspore {
namespace dataset {
namespace vision {
#ifndef ENABLE_ANDROID
BoundingBoxAugmentOperation::BoundingBoxAugmentOperation(const std::shared_ptr<TensorOperation> &transform, float ratio)
    : transform_(transform), ratio_(ratio) {}

BoundingBoxAugmentOperation::~BoundingBoxAugmentOperation() = default;

std::string BoundingBoxAugmentOperation::Name() const { return kBoundingBoxAugmentOperation; }

Status BoundingBoxAugmentOperation::ValidateParams() {
  RETURN_IF_NOT_OK(ValidateVectorTransforms("BoundingBoxAugment", {transform_}));
  RETURN_IF_NOT_OK(ValidateScalar("BoundingBoxAugment", "ratio", ratio_, {0.0, 1.0}, false, false));
  return Status::OK();
}

std::shared_ptr<TensorOp> BoundingBoxAugmentOperation::Build() {
  std::shared_ptr<BoundingBoxAugmentOp> tensor_op = std::make_shared<BoundingBoxAugmentOp>(transform_->Build(), ratio_);
  return tensor_op;
}

Status BoundingBoxAugmentOperation::to_json(nlohmann::json *out_json) {
  nlohmann::json args, transform_args;
  RETURN_IF_NOT_OK(transform_->to_json(&transform_args));
  args["transform"] = transform_args;
  args["ratio"] = ratio_;
  *out_json = args;
  return Status::OK();
}
#endif
}  // namespace vision
}  // namespace dataset
}  // namespace mindspore
