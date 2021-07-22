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

#include "tools/optimizer/common/format_utils.h"
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include "ops/adam.h"
#include "ops/addn.h"
#include "ops/apply_momentum.h"
#include "ops/batch_norm.h"
#include "ops/batch_to_space.h"
#include "ops/bias_add.h"
#include "ops/concat.h"
#include "ops/crop.h"
#include "ops/depth_to_space.h"
#include "ops/fused_batch_norm.h"
#include "ops/fusion/activation.h"
#include "ops/fusion/add_fusion.h"
#include "ops/fusion/avg_pool_fusion.h"
#include "ops/fusion/conv2d_backprop_input_fusion.h"
#include "ops/fusion/conv2d_backprop_filter_fusion.h"
#include "ops/fusion/conv2d_fusion.h"
#include "ops/fusion/conv2d_transpose_fusion.h"
#include "ops/fusion/div_fusion.h"
#include "ops/fusion/max_pool_fusion.h"
#include "ops/fusion/mul_fusion.h"
#include "ops/fusion/pow_fusion.h"
#include "ops/fusion/prelu_fusion.h"
#include "ops/fusion/slice_fusion.h"
#include "ops/fusion/topk_fusion.h"
#include "ops/eltwise.h"
#include "ops/grad/activation_grad.h"
#include "ops/grad/avg_pool_grad.h"
#include "ops/grad/batch_norm_grad.h"
#include "ops/grad/bias_add_grad.h"
#include "ops/grad/max_pool_grad.h"
#include "ops/grad/resize_grad.h"
#include "ops/instance_norm.h"
#include "ops/lrn.h"
#include "ops/maximum.h"
#include "ops/op_utils.h"
#include "ops/quant_dtype_cast.h"
#include "ops/resize.h"
#include "ops/roi_pooling.h"
#include "ops/sgd.h"
#include "ops/space_to_batch.h"
#include "ops/space_to_batch_nd.h"
#include "ops/space_to_depth.h"
#include "ops/split.h"
#include "ops/strided_slice.h"
#include "tools/anf_exporter/fetch_content.h"

namespace mindspore {
namespace opt {
static const std::unordered_map<std::string, std::vector<size_t>> NHWCOpMap = {
  {ops::kNameAdam, {10}},
  {ops::kNameApplyMomentum, {4}},
  {ops::kNameAvgPoolFusion, {1}},
  {ops::kNameAvgPoolGrad, {}},
  {ops::kNameBatchNorm, {1}},
  {ops::kNameBatchNormGrad, {1, 2}},
  {ops::kNameBatchToSpace, {1}},
  {ops::kNameBiasAdd, {1}},
  {ops::kNameBiasAddGrad, {1}},
  {ops::kNameConv2DBackpropInputFusion, {1}},
  {ops::kNameConv2DBackpropFilterFusion, {1, 2}},
  {ops::kNameConv2DFusion, {1}},
  {ops::kNameConv2dTransposeFusion, {1}},
  {ops::kNameDepthToSpace, {1}},
  {ops::kNameFusedBatchNorm, {1}},
  {ops::kNameLRN, {1}},
  {ops::kNameMaxPoolFusion, {1}},
  {ops::kNameMaxPoolGrad, {}},
  {ops::kNamePReLUFusion, {1}},
  {ops::kNameResize, {1}},
  {ops::kNameResizeGrad, {}},
  {ops::kNameROIPooling, {1}},
  {ops::kNameSGD, {2}},
  {ops::kNameSpaceToBatch, {1}},
  {ops::kNameSpaceToBatchND, {1}},
  {ops::kNameSpaceToDepth, {1}},
  {ops::kNameTopKFusion, {1}}};

static const std::unordered_map<std::string, std::vector<size_t>> NCHWOpMap = {{ops::kNameInstanceNorm, {1}}};

// a certain op whose input's format is not fixed.
static const std::vector<std::string> DynamicFormatOpList = {
  ops::kNameEltwise,      ops::kNameActivation, ops::kNameConcat,  ops::kNameDivFusion,      ops::kNamePowFusion,
  ops::kNameStridedSlice, ops::kNameAddFusion,  ops::kNameAddN,    ops::kNameSplit,          ops::kNameSliceFusion,
  ops::kNameCrop,         ops::kNameMulFusion,  ops::kNameMaximum, ops::kNameActivationGrad, ops::kNameQuantDTypeCast};

static const std::unordered_map<int, int> NC2NHAxisMap = {{0, 0}, {1, 3}, {2, 1}, {3, 2}};

const std::unordered_map<std::string, std::vector<size_t>> &GetNHWCOpMap() { return NHWCOpMap; }
const std::unordered_map<std::string, std::vector<size_t>> &GetNCHWOpMap() { return NCHWOpMap; }
const std::unordered_map<int, int> &GetNC2NHAxisMap() { return NC2NHAxisMap; }
const std::vector<std::string> &GetDynamicFormatOpList() { return DynamicFormatOpList; }

Format GetFormat(const CNodePtr &cnode) {
  MS_ASSERT(cnode != nullptr);
  auto prim_node = cnode->input(0);
  MS_ASSERT(prim_node != nullptr);
  auto prim = GetValueNode<PrimitivePtr>(prim_node);
  MS_ASSERT(prim != nullptr);
  Format format = NHWC;
  if (prim->GetAttr(ops::kFormat) != nullptr) {
    format = static_cast<Format>(GetValue<int64_t>(prim->GetAttr(ops::kFormat)));
  }
  return format;
}

STATUS GetTransposePerm(const CNodePtr &cnode, std::vector<int> *perm) {
  MS_ASSERT(perm_node != nullptr);
  if (cnode->size() != 3) {
    MS_LOG(ERROR) << "transpose op input size must be three.";
    return lite::RET_ERROR;
  }
  if (utils::isa<CNodePtr>(cnode->input(2))) {
    return lite::RET_OK;
  }
  lite::DataInfo data_info;
  int status;
  if (utils::isa<ParameterPtr>(cnode->input(2))) {
    status = lite::FetchDataFromParameterNode(cnode, 2, lite::converter::FmkType_MS, false, &data_info);
  } else {
    status = lite::FetchDataFromValueNode(cnode, 2, lite::converter::FmkType_MS, false, &data_info);
  }
  if (status != lite::RET_OK) {
    MS_LOG(ERROR) << "fetch transpose perm data failed.";
    return lite::RET_ERROR;
  }
  if ((data_info.data_type_ != kNumberTypeInt && data_info.data_type_ != kNumberTypeInt32) ||
      data_info.shape_.size() != 1) {
    MS_LOG(ERROR) << "transpose perm data is invalid.";
    return lite::RET_ERROR;
  }
  perm->resize(data_info.shape_[0]);
  if (!data_info.data_.empty() &&
      memcpy_s(perm->data(), data_info.data_.size(), data_info.data_.data(), data_info.data_.size()) != EOK) {
    MS_LOG(ERROR) << "memcpy data failed.";
    return lite::RET_ERROR;
  }
  return lite::RET_OK;
}

void RemoveIfMonad(const CNodePtr &cnode) {
  MS_ASSERT(cnode != nullptr);
  std::vector<AnfNodePtr> inputs{cnode->input(0)};
  for (size_t i = 1; i < cnode->size(); ++i) {
    if (utils::isa<ValueNodePtr>(cnode->input(i))) {
      auto value_node = cnode->input(i)->cast<ValueNodePtr>();
      auto value = value_node->value();
      if (value->isa<Monad>()) {
        continue;
      }
    }
    inputs.push_back(cnode->input(i));
  }
  cnode->set_inputs(inputs);
}

bool IsMonadNode(const AnfNodePtr &node) {
  if (!utils::isa<ValueNodePtr>(node)) {
    return false;
  }
  auto value_node = node->cast<ValueNodePtr>();
  auto value = value_node->value();
  if (value->isa<Monad>()) {
    return true;
  }
  return false;
}

bool IsSpecialType(const CNodePtr &cnode) {
  return CheckPrimitiveType(cnode, prim::kPrimTupleGetItem) || CheckPrimitiveType(cnode, prim::kPrimDepend) ||
         CheckPrimitiveType(cnode, prim::kPrimMakeTuple) || CheckPrimitiveType(cnode, kPrimMakeTupleV2) ||
         CheckPrimitiveType(cnode, prim::kPrimReturn);
}
}  // namespace opt
}  // namespace mindspore
