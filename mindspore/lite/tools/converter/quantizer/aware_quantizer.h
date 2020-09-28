/**
 * Copyright 2019 Huawei Technologies Co., Ltd
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

#ifndef MS_AWARE_QUANTIZER_H
#define MS_AWARE_QUANTIZER_H

#include <array>
#include <string>
#include <memory>
#include "tools/converter/quantizer/quantizer.h"
#include "schema/inner/model_generated.h"
#include "include/errorcode.h"
#include "tools/converter/quantizer/quantize_util.h"

namespace mindspore::lite::quant {
struct InputArray {
  std::unique_ptr<schema::QuantParamT> quantParam;
  float mMin = 0.0f;
  float mMax = 0.0f;
  bool narrowRange = false;
  int numBits = 8;
  TypeId dataType = TypeId::kTypeUnknown;

  InputArray(float mean, float stdDev, TypeId dataType = TypeId::kNumberTypeFloat) {
    this->dataType = dataType;
    constexpr float qmin = -128;
    constexpr float qmax = 127;
    mMin = (qmin - mean) / stdDev;
    mMax = (qmax - mean) / stdDev;
  }

  STATUS InitQuantParam();
  STATUS SetInputArrayQP(schema::MetaGraphT *graph, size_t inputTensorIdx);
};

class AwareQuantizer : public FbQuantizer {
 public:
  AwareQuantizer(schema::MetaGraphT *graph, const TypeId &inferType, const std::string &stdValues,
                 const std::string &meanValues);

  ~AwareQuantizer() { delete (mInputArray); }

  STATUS RemoveFakeQuant() override;

  STATUS GenerateQuantParam() override;

  STATUS DetermineNodeQuantType() override;

  STATUS DoQuantize() override;  // override;

 private:
  // RemoveFakeQuant
  STATUS SetAttrToConvolution(const schema::MetaGraphT *subGraph, schema::CNodeT *node);

  STATUS GenerateDefaultQuantParam(const schema::MetaGraphT *subGraph);

  STATUS QuantArithmeticConstTensor(const schema::MetaGraphT *graph, schema::CNodeT *node);

  STATUS QuantDetectionPostProcessConstTensor(const schema::MetaGraphT *subGraph, schema::CNodeT *node);

  STATUS QuantConvBias(const schema::MetaGraphT *graph, schema::CNodeT *node);

  STATUS QuantConvWeight(const schema::MetaGraphT *subGraph, schema::CNodeT *node);

  float inputScale = 0.0f;

  InputArray *mInputArray;

  static const std::array<schema::PrimitiveType, 7> propagatedOps;
};
}  // namespace mindspore::lite::quant
#endif
