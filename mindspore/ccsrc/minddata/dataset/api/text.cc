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

#include "minddata/dataset/include/text.h"
#include "minddata/dataset/text/kernels/lookup_op.h"

namespace mindspore {
namespace dataset {
namespace api {
namespace text {

std::shared_ptr<LookupOperation> Lookup(const std::shared_ptr<Vocab> &vocab, const std::string &unknown_token,
                                        const DataType &data_type) {
  auto op = std::make_shared<LookupOperation>(vocab, unknown_token, data_type);

  if (!op->ValidateParams()) {
    return nullptr;
  }
  return op;
}

// LookupOperation
LookupOperation::LookupOperation(const std::shared_ptr<Vocab> &vocab, const std::string &unknown_token,
                                 const DataType &data_type)
    : vocab_(vocab), unknown_token_(unknown_token), default_id_(Vocab::kNoTokenExists), data_type_(data_type) {}

bool LookupOperation::ValidateParams() {
  if (vocab_ == nullptr) {
    MS_LOG(ERROR) << "Lookup: vocab object type is incorrect or null.";
    return false;
  }
  default_id_ = vocab_->Lookup(unknown_token_);
  if (default_id_ == Vocab::kNoTokenExists) {
    MS_LOG(ERROR) << "Lookup: " << unknown_token_ << " doesn't exist in vocab.";
    return false;
  }
  return true;
}

std::shared_ptr<TensorOp> LookupOperation::Build() {
  std::shared_ptr<LookupOp> tensor_op = std::make_shared<LookupOp>(vocab_, default_id_, data_type_);
  return tensor_op;
}

}  // namespace text
}  // namespace api
}  // namespace dataset
}  // namespace mindspore
