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
#ifndef MINDSPORE_LITE_TOOLS_CONVERTER_PARSER_TFLITE_MODEL_PARSER_H
#define MINDSPORE_LITE_TOOLS_CONVERTER_PARSER_TFLITE_MODEL_PARSER_H

#include <string>
#include <unordered_map>
#include <memory>
#include <vector>
#include <set>
#include "tools/converter/model_parser.h"
#include "include/registry/model_parser_registry.h"
#include "tools/converter/parser/tflite/tflite_node_parser_registry.h"
#include "tools/common/tensor_util.h"

namespace mindspore {
namespace lite {
class TfliteModelParser : public ModelParser {
 public:
  TfliteModelParser() = default;

  ~TfliteModelParser() override = default;

  FuncGraphPtr Parse(const converter::ConverterParameters &flag) override;

  static int Tflite2AnfAdjust(const std::set<FuncGraphPtr> &all_func_graphs);

 private:
  std::unordered_map<int, AnfNodePtr> nodes_;
  std::unique_ptr<tflite::ModelT> tflite_model_;
  char *tflite_model_buf_ = nullptr;
  std::unique_ptr<tflite::ModelT> ReadTfliteModel(const std::string &model_path);
  STATUS ConvertConstTensor(const tflite::TensorT *tensor, const ParameterPtr &parameter,
                            const std::string &tensor_name);
  STATUS ConvertOutputTensor(const tflite::OperatorT *op, const CNodePtr &dst_cnode);
  STATUS ConvertOpQuantParams(const tflite::OperatorT *op, ops::PrimitiveC *primitive_c);
  STATUS ConvertOps();
  STATUS ConvertGraphInputs();
  STATUS ConvertGraphOutputs();
  static STATUS SetTensorQuantParam(const tflite::TensorT *tflite_tensor, std::vector<QuantParamT> *quant_params,
                                    int round_type = 1);
  int DoWeightFormatTransform(const CNodePtr &conv_node, const AnfNodePtr &weight_node, const FuncGraphPtr &graph,
                              schema::Format weight_src_format, schema::Format weight_dst_format);
  STATUS WeightFormatTransform(const FuncGraphPtr &graph);
  STATUS HardCodeTflite(const CNodePtr &conv_node, const tensor::TensorPtr &tensor_info, const FuncGraphPtr &graph);
  QuantType quant_type_ = schema::QuantType_QUANT_NONE;
};
}  // namespace lite
}  // namespace mindspore
#endif  // MINDSPORE_LITE_TOOLS_CONVERTER_PARSER_TFLITE_MODEL_PARSER_H
