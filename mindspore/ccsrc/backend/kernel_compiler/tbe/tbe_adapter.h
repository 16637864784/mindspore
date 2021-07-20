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
#ifndef MINDSPORE_CCSRC_BACKEND_KERNEL_COMPILER_TBE_TBE_ADAPTER_H
#define MINDSPORE_CCSRC_BACKEND_KERNEL_COMPILER_TBE_TBE_ADAPTER_H

#include <string>
#include <map>
#include <memory>
#include <vector>
#include <algorithm>
#include <unordered_set>
#include "nlohmann/json.hpp"
#include "base/base.h"
#include "backend/kernel_compiler/oplib/opinfo.h"
#include "backend/kernel_compiler/kernel_fusion.h"
// Note: This file is mainly used to adapt the ME front-end operator description and
//       the TBE back-end operator implementation difference
namespace mindspore {
namespace kernel {
constexpr size_t INPUT0 = 0;
constexpr size_t INPUT1 = 1;
constexpr size_t INPUT2 = 2;
constexpr size_t INPUT3 = 3;
constexpr size_t INPUT4 = 4;
constexpr size_t INPUT5 = 5;
constexpr size_t INPUT6 = 6;
constexpr size_t INPUT7 = 7;
constexpr size_t INPUT8 = 8;
enum kCreaterType : int { SINGLE_BUILD = 0, OP_SELECT_FORMAT, CHECK_SUPPORTED, OP_PRE_COMPILE };
namespace tbe {
const std::map<std::string, std::string> opTypeAdapter = {{"ReLUV2", "ReluV2"},
                                                          {"ReLU6", "Relu6"},
                                                          {"ReLU6Grad", "Relu6Grad"},
                                                          {"ReLUGrad", "ReluGrad"},
                                                          {"ReLU", "Relu"},
                                                          {"Gather", "GatherV2"},
                                                          {"SparseApplyFtrl", "SparseApplyFtrlD"},
                                                          {"Concat", "ConcatD"},
                                                          {"DepthwiseConv2dNative", "DepthwiseConv2D"},
                                                          {"FastGeLU", "FastGelu"},
                                                          {"FastGeLUGrad", "FastGeluGrad"},
                                                          {"GeLU", "Gelu"},
                                                          {"GeLUGrad", "GeluGrad"},
                                                          {"PReLU", "PRelu"},
                                                          {"PReLUGrad", "PReluGrad"},
                                                          {"SeLU", "Selu"}};

enum FusionDataType { kFusionNormal = 0, kFusionAddN, kFusionReLUGradV2, kFusionAdd };
using FAttrsPass = void (*)(const AnfNodePtr &anf_node, const std::vector<std::shared_ptr<OpAttr>> &op_info_attrs,
                            nlohmann::json *attrs_json);
using FPreAttrsPass = void (*)(const AnfNodePtr &anf_node, std::vector<OpAttrPtr> *op_info_attrs,
                               nlohmann::json *attrs_json);
class TbeAdapter {
 public:
  TbeAdapter() = default;
  ~TbeAdapter() = default;
  template <typename T>
  static void InputOrderPass(const std::string &op_name, std::vector<T> const &inputs_list,
                             std::vector<T> *inputs_json) {
    MS_EXCEPTION_IF_NULL(inputs_json);
    if (input_order_adjusted_ops_.find(op_name) == input_order_adjusted_ops_.end()) {
      (void)std::copy(inputs_list.begin(), inputs_list.end(), std::back_inserter((*inputs_json)));
    } else {
      if (op_name == kMinimumGradOpName || op_name == kMaximumGradOpName) {
        inputs_json->push_back(inputs_list[INPUT2]);
        inputs_json->push_back(inputs_list[INPUT0]);
        inputs_json->push_back(inputs_list[INPUT1]);
        for (size_t i = 3; i < inputs_list.size(); ++i) {
          inputs_json->push_back(inputs_list[i]);
        }
      } else if (op_name == kApplyCenteredRMSPropOpName) {
        // Parameter order of ApplyCenteredRMSProp's TBE implementation is different from python API, so map
        // TBE parameter to correspond python API parameter by latter's index using hardcode
        inputs_json->push_back(inputs_list[INPUT0]);
        inputs_json->push_back(inputs_list[INPUT1]);
        inputs_json->push_back(inputs_list[INPUT2]);
        inputs_json->push_back(inputs_list[INPUT3]);
        inputs_json->push_back(inputs_list[INPUT5]);
        inputs_json->push_back(inputs_list[INPUT6]);
        inputs_json->push_back(inputs_list[INPUT7]);
        inputs_json->push_back(inputs_list[INPUT8]);
        inputs_json->push_back(inputs_list[INPUT4]);
      } else {
        inputs_json->push_back(inputs_list[INPUT1]);
        inputs_json->push_back(inputs_list[INPUT0]);
        for (size_t i = 2; i < inputs_list.size(); ++i) {
          inputs_json->push_back(inputs_list[i]);
        }
      }
    }
  }

  // TODO(xxx): delete
  //  FusionInputOrderPass/InputOrderPass/FusionDataOrderPass/GenTopKV2IndicesTensorInfo/GetNodeFusionType
  static void FusionInputOrderPass(const std::string &op_name, const std::vector<nlohmann::json> &inputs_list,
                                   std::vector<nlohmann::json> *inputs_json);
  static void InputOrderPass(const std::string &op_name, std::vector<std::vector<nlohmann::json>> const &inputs_list,
                             nlohmann::json *inputs_json);
  static void FusionDataOrderPass(const std::string &op_name, const std::vector<AnfNodePtr> &data_layer,
                                  std::vector<AnfNodePtr> *reorder_data_layer);
  static void GenTopKV2IndicesTensorInfo(const std::shared_ptr<AnfNode> &anf_node, size_t real_input_index,
                                         std::vector<nlohmann::json> *input_list, kCreaterType creater_type);
  static std::string GetNodeFusionType(const mindspore::CNodePtr &cnode);

  static bool RunAttrPass(const AnfNodePtr &anf_node, const std::vector<std::shared_ptr<OpAttr>> &op_info_attrs,
                          nlohmann::json *attrs_json);
  static void FusionDescJsonPass(const AnfNodePtr &node, nlohmann::json *output_desc,
                                 const std::map<const AnfNodePtr, tbe::FusionDataType> &spec_data_input);
  static std::string GetRealOpType(const std::string &origin_type);
  static std::string FormatPass(const std::string &format, const size_t &origin_shape_size);
  static bool GetSpecDataInput(const FusionScopeInfo &fusion_scope_info,
                               std::map<const AnfNodePtr, tbe::FusionDataType> *spec_data_input);
  static bool IsPlaceHolderInput(const AnfNodePtr &node, const OpIOInfoPtr &input_ptr);
  static void CastAttrJsonPrePass(const AnfNodePtr &anf_node, std::vector<OpAttrPtr> *op_info_attrs,
                                  nlohmann::json *attrs_json);
  static void CastJsonPostPass(const AnfNodePtr &anf_node, nlohmann::json *attrs_json);

 private:
  // TODO(xxx): delete MaxiOrMinimumGradAttrJsonPass
  static void MaxiOrMinimumGradAttrJsonPass(const AnfNodePtr &anf_node,
                                            const std::vector<std::shared_ptr<OpAttr>> &op_info_attrs,
                                            nlohmann::json *attrs_json);
  static void CastAttrJsonPass(const AnfNodePtr &anf_node, const std::vector<std::shared_ptr<OpAttr>> &op_info_attrs,
                               nlohmann::json *attrs_json);

  static bool IsSpecialFusionComputeNode(const std::vector<mindspore::AnfNodePtr> &compute_nodes);
  static bool GetSpecInputLayers(const std::string &op_name, const std::vector<mindspore::AnfNodePtr> &reorder_layer,
                                 std::map<const AnfNodePtr, FusionDataType> *spec_data_input);

  static std::map<std::string, FAttrsPass> build_json_attr_pass_map_;
  static std::unordered_set<std::string> input_order_adjusted_ops_;
};
}  // namespace tbe
}  // namespace kernel
}  // namespace mindspore
#endif  // MINDSPORE_CCSRC_BACKEND_KERNEL_COMPILER_TBE_TBE_ADAPTER_H
