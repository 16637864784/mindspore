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

#ifndef LITE_RETURN_CODE_H
#define LITE_RETURN_CODE_H

#include <string>
#include <set>
#include "include/errorcode.h"
#include "src/common/log_adapter.h"

namespace mindspore {
namespace lite {
class ReturnCode {
 public:
  ~ReturnCode() = default;
  static ReturnCode *GetSingleReturnCode() {
    static ReturnCode returnCode;
    return &returnCode;
  }
  void UpdateReturnCode(STATUS status) {
    if (statusCode == RET_OK) {
      statusCode = status;
    }
  }
  STATUS GetReturnCode() const { return statusCode; }

 private:
  ReturnCode() { statusCode = RET_OK; }
  int statusCode;
};

class NoSupportOp {
 public:
  ~NoSupportOp() = default;
  static NoSupportOp *GetInstance() {
    static NoSupportOp noSupportOp;
    return &noSupportOp;
  }
  void InsertOp(const std::string &op_name) { noSupportOps.insert(op_name); }
  void PrintOps() const {
    for (auto &op_name : noSupportOps) {
      MS_LOG(ERROR) << "The op " << op_name << " hasn't been supported";
    }
  }

 private:
  NoSupportOp() { noSupportOps.clear(); }
  std::set<std::string> noSupportOps;
};
}  // namespace lite
}  // namespace mindspore
#endif  // LITE_RETURN_CODE_H
