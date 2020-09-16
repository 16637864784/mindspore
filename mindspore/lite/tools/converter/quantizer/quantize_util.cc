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

#include "mindspore/lite/tools/converter/quantizer/quantize_util.h"
#include <cmath>
#include <string>
#include <algorithm>
#include <memory>
#include <vector>
#include "src/ops/primitive_c.h"
#include "mindspore/lite/tools/converter/quantizer/general_bitpacking.h"
#include "src/common/utils.h"
#include "abstract/abstract_value.h"
#include "securec/include/securec.h"

using std::string;
using std::vector;

namespace mindspore {
namespace lite {
namespace quant {
const std::vector<schema::PrimitiveType> QuantStrategy::conv_types = {
  schema::PrimitiveType_DeConv2D, schema::PrimitiveType_DeDepthwiseConv2D,
  schema::PrimitiveType_Conv2D,    schema::PrimitiveType_DepthwiseConv2D};
const std::vector<schema::PrimitiveType> QuantStrategy::mul_types = {
  schema::PrimitiveType_Mul, schema::PrimitiveType_MatMul, schema::PrimitiveType_FullConnection};
QuantStrategy::QuantStrategy(size_t weightSize, size_t convWeightQuantChannelThreshold)
    : mWeightSize(weightSize), mConvWeightQuantChannelThreshold(convWeightQuantChannelThreshold) {}

bool QuantStrategy::CanConvOpQuantized(const CNodePtr &node) const {
  auto primitive_c = GetValueNode<std::shared_ptr<PrimitiveC>>(node->input(0));
  if (primitive_c == nullptr) {
    MS_LOG(ERROR) << "primitive_c is nullptr";
    return false;
  }
  if (!IsContain(conv_types, (schema::PrimitiveType)primitive_c->Type())) {
    return false;
  }
  if (node->size() < 3) {
    return false;
  }

  auto inputNode = node->input(2);
  if (!inputNode->isa<Parameter>()) {
    return false;
  }
  auto paramNode = inputNode->cast<ParameterPtr>();
  auto abstract_base = paramNode->abstract();
  if (abstract_base == nullptr) {
    return false;
  }

  if (!utils::isa<abstract::ShapePtr>(abstract_base->GetShapeTrack())) {
    MS_LOG(INFO) << "Shape of Abstract of parameter should be ShapePtr " << paramNode->name();
    return false;
  }
  auto weight_shape = utils::cast<abstract::ShapePtr>(abstract_base->GetShapeTrack())->shape();
  size_t shapeSize = 1;
  for (auto dim : weight_shape) {
    shapeSize = shapeSize * dim;
  }
  if (shapeSize < mWeightSize) {
    MS_LOG(INFO) << "shapeSize Invalid!" << shapeSize;
    return false;
  }
  if (weight_shape[0] <= static_cast<int>(mConvWeightQuantChannelThreshold)) {
    MS_LOG(INFO) << "channel less mConvWeightQuantChannelThreshold!" << weight_shape[0];
    return false;
  }

  return true;
}

bool QuantStrategy::CanOpPostQuantized(AnfNodePtr &node) const {
  if (!node->isa<CNode>()) {
    return false;
  }
  auto cnode = std::dynamic_pointer_cast<CNode>(node);

  auto primitive_c = GetValueNode<std::shared_ptr<PrimitiveC>>(cnode->input(0));
  if (primitive_c == nullptr) {
    MS_LOG(WARNING) << "primitive_c is nullptr: " << cnode->fullname_with_scope();
    return false;
  }

  auto type = (schema::PrimitiveType)primitive_c->Type();
  MS_LOG(INFO) << "Primitive type: " << type;
  static const std::vector<schema::PrimitiveType> uint8OpList = {
    schema::PrimitiveType_Nchw2Nhwc, schema::PrimitiveType_Nhwc2Nchw,
    schema::PrimitiveType_Conv2D,    schema::PrimitiveType_DepthwiseConv2D,
    schema::PrimitiveType_Add,       schema::PrimitiveType_Pooling,
    schema::PrimitiveType_Concat, /*schema::PrimitiveType_SoftMax,*/
    schema::PrimitiveType_Reshape,   schema::PrimitiveType_FullConnection,
    schema::PrimitiveType_MatMul,    schema::PrimitiveType_Activation};
  return IsContain(uint8OpList, type);
}

bool QuantStrategy::CanMulOpQuantized(const CNodePtr &node) const {
  auto primitive_c = GetValueNode<std::shared_ptr<PrimitiveC>>(node->input(0));
  if (primitive_c == nullptr) {
    MS_LOG(ERROR) << "primitive_c is nullptr";
    return false;
  }

  if (!IsContain(mul_types, (schema::PrimitiveType)primitive_c->Type())) {
    return false;
  }

  if (node->size() < 3) {
    MS_LOG(INFO) << "input size less!";
    return false;
  }

  auto inputNode1 = node->input(1);
  auto inputNode2 = node->input(2);
  if (inputNode1 == nullptr || inputNode2 == nullptr) {
    MS_LOG(INFO) << "mul input is nullptr!";
    return false;
  }

  ParameterPtr paramNode = nullptr;
  if (inputNode1->isa<Parameter>()) {
    paramNode = inputNode1->cast<ParameterPtr>();
  } else if (inputNode2->isa<Parameter>()) {
    paramNode = inputNode2->cast<ParameterPtr>();
  }

  if (paramNode == nullptr) {
    MS_LOG(INFO) << "invalid paramNode!";
    return false;
  }

  auto abstract_base = paramNode->abstract();
  if (abstract_base == nullptr) {
    MS_LOG(INFO) << "abstract is nullptr";
    return false;
  }

  if (!utils::isa<abstract::ShapePtr>(abstract_base->GetShapeTrack())) {
    MS_LOG(INFO) << "Shape of Abstract of parameter should be ShapePtr " << paramNode->name();
    return false;
  }
  auto weight_shape = utils::cast<abstract::ShapePtr>(abstract_base->GetShapeTrack())->shape();
  size_t shapeSize = 1;
  for (auto dim : weight_shape) {
    shapeSize = shapeSize * dim;
  }
  if (shapeSize < mWeightSize) {
    MS_LOG(INFO) << "shapeSize Invalid!" << shapeSize;
    return false;
  }

  return true;
}

STATUS CalQuantizationParams(schema::QuantParamT *quantParam, double mMin, double mMax, bool narrowRange, int quant_max,
                             int quant_min, int num_bits) {
  MS_ASSERT(quantParam != nullptr);
  if (mMin > 0.0f) {
    MS_LOG(DEBUG) << "min " << mMin << " is bigger then 0, set to 0, this may course low precision";
    mMin = 0.0f;
  }
  if (mMax < 0.0f) {
    MS_LOG(DEBUG) << "mMax " << mMax << " is smaller than 0, set to 0, this may course low precision";
    mMax = 0.0f;
  }
  if (mMin > mMax) {
    MS_LOG(ERROR) << "cal error while min" << mMin << ">" << mMax;
    return RET_PARAM_INVALID;
  }
  if (mMin == mMax) {
    if (mMin != 0.0f) {
      MS_LOG(ERROR) << "min and max should both be zero if they are equal to each other";
      return RET_ERROR;
    }
    quantParam->inited = true;
    quantParam->min = mMin;
    quantParam->max = mMax;
    quantParam->scale = 0.0f;
    quantParam->zeroPoint = 0;
    quantParam->narrowRange = narrowRange;
    quantParam->numBits = num_bits;
    return RET_OK;
  }

  auto quantMinFloat = static_cast<double>(quant_min);
  auto quantMaxFloat = static_cast<double>(quant_max);
  double scale = (mMax - mMin) / (quantMaxFloat - quantMinFloat);
  const double zeroPointFromMin = quantMinFloat - mMin / scale;
  int zeroPoint = static_cast<int32_t>(std::round(zeroPointFromMin));

  // The zero point should always be in the range of quantized value,
  // [qmin, qmax].
  MS_ASSERT(zeroPoint >= quantMin);
  MS_ASSERT(zeroPoint <= quantMax);
  quantParam->inited = true;
  quantParam->min = mMin;
  quantParam->max = mMax;
  quantParam->scale = scale;
  quantParam->zeroPoint = zeroPoint;
  quantParam->narrowRange = narrowRange;
  quantParam->numBits = num_bits;

  return RET_OK;
}

STATUS CalQuantizationParams(schema::QuantParamT *quantParam, double mMin, double mMax, bool narrowRange, int numBits) {
  MS_ASSERT(quantParam != nullptr);
  if (mMin > 0.0f) {
    MS_LOG(DEBUG) << "min " << mMin << " is bigger then 0, set to 0, this may course low precision";
    mMin = 0.0f;
  }
  if (mMax < 0.0f) {
    MS_LOG(DEBUG) << "mMax " << mMax << " is smaller than 0, set to 0, this may course low precision";
    mMax = 0.0f;
  }
  if (mMin > mMax) {
    MS_LOG(ERROR) << "cal error while min" << mMin << ">" << mMax;
    return RET_PARAM_INVALID;
  }
  if (mMin == mMax) {
    if (mMin != 0.0f) {
      MS_LOG(ERROR) << "min and max should both be zero if they are equal to each other";
      return RET_ERROR;
    }
    quantParam->inited = true;
    quantParam->min = mMin;
    quantParam->max = mMax;
    quantParam->scale = 0.0f;
    quantParam->zeroPoint = 0;
    quantParam->narrowRange = narrowRange;
    quantParam->numBits = numBits;
    return RET_OK;
  }

  const int8_t quantMin = std::numeric_limits<int8_t>::min() + (narrowRange ? 1 : 0);
  const int8_t quantMax = std::numeric_limits<int8_t>::max();
  auto quantMinFloat = static_cast<double>(quantMin);
  auto quantMaxFloat = static_cast<double>(quantMax);
  double scale = (mMax - mMin) / (quantMaxFloat - quantMinFloat);
  const double zeroPointFromMin = quantMinFloat - mMin / scale;
  const double zeroPointFromMax = quantMaxFloat - mMax / scale;
  const double zpFromMinError = std::abs(quantMinFloat) + std::abs(mMin / scale);
  const double zpFromMaxError = std::abs(quantMaxFloat) + std::abs(mMax / scale);
  const double zpDouble = zpFromMinError < zpFromMaxError ? zeroPointFromMin : zeroPointFromMax;
  int zeroPoint;
  if (zpDouble < quantMinFloat) {
    zeroPoint = quantMin;
  } else if (zpDouble > quantMaxFloat) {
    zeroPoint = quantMax;
  } else {
    zeroPoint = static_cast<int32_t>(std::round(zpDouble));
  }
  if (std::abs(mMin) == std::abs(mMax)) {
    zeroPoint = 0;
  }
  // The zero point should always be in the range of quantized value,
  // [qmin, qmax].
  MS_ASSERT(zeroPoint >= quantMin);
  MS_ASSERT(zeroPoint <= quantMax);
  quantParam->inited = true;
  quantParam->min = mMin;
  quantParam->max = mMax;
  quantParam->scale = scale;
  quantParam->zeroPoint = zeroPoint;
  quantParam->narrowRange = narrowRange;
  quantParam->numBits = numBits;

  return RET_OK;
}

STATUS PostBitPack(float *weight, size_t shapeSize, size_t bitNum) {
  auto *rawDatas = reinterpret_cast<uint8_t *>(weight);
  vector<uint8_t> qDatas(rawDatas, rawDatas + shapeSize);
  vector<uint8_t> qDatas_packed;
  if (bitNum < 8 && bitNum > 1) {
    BitPack weight_bitpack(bitNum);
    weight_bitpack.BitPacking(qDatas, qDatas_packed);
    if (EOK != memcpy_s(rawDatas, shapeSize, &qDatas_packed[0], shapeSize)) {
      MS_LOG(ERROR) << "PostBitPack memcpy_s qDatas_packed failed";
      return RET_ERROR;
    }
  } else if (bitNum == 8) {
    if (EOK != memcpy_s(rawDatas, shapeSize, &qDatas[0], shapeSize)) {
      MS_LOG(ERROR) << "PostBitPack memcpy_s qDatas failed";
      return RET_ERROR;
    }
  } else {
    MS_LOG(ERROR) << "bitNum must be between 0 and 8 : " << bitNum;
    return RET_ERROR;
  }

  return RET_OK;
}
}  // namespace quant
}  // namespace lite
}  // namespace mindspore
