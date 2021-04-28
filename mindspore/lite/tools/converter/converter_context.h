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

#ifndef MINDSPORE_LITE_TOOLS_CONVERTER_RETURN_CODE_H
#define MINDSPORE_LITE_TOOLS_CONVERTER_RETURN_CODE_H

#include <string>
#include <set>
#include <map>
#include "include/errorcode.h"
#include "src/common/log_adapter.h"
#include "ir/dtype/type_id.h"

namespace mindspore {
namespace lite {
class ReturnCode {
 public:
  static ReturnCode *GetSingleReturnCode() {
    static ReturnCode return_code;
    return &return_code;
  }
  void UpdateReturnCode(STATUS status) {
    if (status_code_ == RET_OK) {
      status_code_ = status;
    }
  }
  STATUS status_code() const { return status_code_; }

 private:
  ReturnCode() = default;
  virtual ~ReturnCode() = default;
  int status_code_ = RET_OK;
};

class NotSupportOp {
 public:
  static NotSupportOp *GetInstance() {
    static NotSupportOp not_support_op;
    return &not_support_op;
  }
  void set_fmk_type(const std::string &fmk_type) { fmk_type_ = fmk_type; }
  void InsertOp(const std::string &op_name) { not_support_ops_.insert(op_name); }
  void PrintOps() const {
    if (!not_support_ops_.empty()) {
      MS_LOG(ERROR) << "===========================================";
      MS_LOG(ERROR) << "UNSUPPORTED OP LIST:";
      for (auto &op_name : not_support_ops_) {
        MS_LOG(ERROR) << "FMKTYPE: " << fmk_type_ << ", OP TYPE: " << op_name;
      }
      MS_LOG(ERROR) << "===========================================";
    }
  }

 private:
  NotSupportOp() = default;
  virtual ~NotSupportOp() = default;
  std::set<std::string> not_support_ops_;
  std::string fmk_type_;
};

class TensorDataType {
 public:
  static TensorDataType *GetInstance() {
    static TensorDataType tensor_data_type;
    return &tensor_data_type;
  }

  void UpdateGraphInputDType(int32_t index, int32_t dtype) { graph_input_data_type_map_[index] = dtype; }
  int32_t GetGraphInputDType(int32_t index) const {
    if (graph_input_data_type_map_.find(index) == graph_input_data_type_map_.end()) {
      return TypeId::kTypeUnknown;
    }
    return graph_input_data_type_map_.at(index);
  }

  void UpdateGraphOutputDType(int32_t index, int32_t dtype) { graph_output_data_type_map_[index] = dtype; }
  int32_t GetGraphOutputDType(int32_t index) const {
    if (graph_output_data_type_map_.find(index) == graph_output_data_type_map_.end()) {
      return TypeId::kTypeUnknown;
    }
    return graph_output_data_type_map_.at(index);
  }

 private:
  TensorDataType() {}
  virtual ~TensorDataType() = default;
  std::map<int32_t, int32_t> tensor_data_type_map_;
  std::map<int32_t, int32_t> graph_input_data_type_map_;
  std::map<int32_t, int32_t> graph_output_data_type_map_;
};
}  // namespace lite
}  // namespace mindspore
#endif  // MINDSPORE_LITE_TOOLS_CONVERTER_RETURN_CODE_H
