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

#include "minddata/dataset/engine/ir/datasetops/source/samplers/weighted_random_sampler_ir.h"
#include "minddata/dataset/engine/datasetops/source/sampler/weighted_random_sampler.h"

#include "minddata/dataset/core/config_manager.h"

namespace mindspore {
namespace dataset {

// Constructor
WeightedRandomSamplerObj::WeightedRandomSamplerObj(std::vector<double> weights, int64_t num_samples, bool replacement)
    : weights_(std::move(weights)), num_samples_(num_samples), replacement_(replacement) {}

// Destructor
WeightedRandomSamplerObj::~WeightedRandomSamplerObj() = default;

Status WeightedRandomSamplerObj::ValidateParams() {
  if (weights_.empty()) {
    RETURN_STATUS_UNEXPECTED("WeightedRandomSampler: weights vector must not be empty");
  }
  int32_t zero_elem = 0;
  for (int32_t i = 0; i < weights_.size(); ++i) {
    if (weights_[i] < 0) {
      RETURN_STATUS_UNEXPECTED("WeightedRandomSampler: weights vector must not contain negative number, got: " +
                               std::to_string(weights_[i]));
    }
    if (weights_[i] == 0.0) {
      zero_elem++;
    }
  }
  if (zero_elem == weights_.size()) {
    RETURN_STATUS_UNEXPECTED("WeightedRandomSampler: elements of weights vector must not be all zero");
  }
  if (num_samples_ < 0) {
    RETURN_STATUS_UNEXPECTED("WeightedRandomSampler: num_samples must be greater than or equal to 0, but got: " +
                             std::to_string(num_samples_));
  }
  return Status::OK();
}

Status WeightedRandomSamplerObj::to_json(nlohmann::json *const out_json) {
  nlohmann::json args;
  args["sampler_name"] = "WeightedRandomSampler";
  args["weights"] = weights_;
  args["num_samples"] = num_samples_;
  args["replacement"] = replacement_;
  if (!children_.empty()) {
    std::vector<nlohmann::json> children_args;
    for (auto child : children_) {
      nlohmann::json child_arg;
      RETURN_IF_NOT_OK(child->to_json(&child_arg));
      children_args.push_back(child_arg);
    }
    args["child_sampler"] = children_args;
  }
  *out_json = args;
  return Status::OK();
}

Status WeightedRandomSamplerObj::SamplerBuild(std::shared_ptr<SamplerRT> *sampler) {
  *sampler = std::make_shared<dataset::WeightedRandomSamplerRT>(num_samples_, weights_, replacement_);
  Status s = BuildChildren(sampler);
  sampler = s.IsOk() ? sampler : nullptr;
  return s;
}
std::shared_ptr<SamplerObj> WeightedRandomSamplerObj::SamplerCopy() {
  auto sampler = std::make_shared<WeightedRandomSamplerObj>(weights_, num_samples_, replacement_);
  for (const auto &child : children_) {
    Status rc = sampler->AddChildSampler(child);
    if (rc.IsError()) MS_LOG(ERROR) << "Error in copying the sampler. Message: " << rc;
  }
  return sampler;
}

}  // namespace dataset
}  // namespace mindspore
