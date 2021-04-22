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
#include "tools/optimizer/graph/mindir_adjust_pass.h"
#include <vector>
#include <memory>

#include "tools/converter/converter_context.h"
#include "tools/converter/quant_param_holder.h"
#include "tools/converter/quantizer/quantize_util.h"
#include "src/common/log_adapter.h"

namespace mindspore {
namespace opt {
namespace {
size_t GetCNodeOutputsSize(std::shared_ptr<AnfNode> anf_node) {
  auto cnode = anf_node->cast<CNodePtr>();
  if (utils::isa<abstract::AbstractTuple>(cnode->abstract())) {
    auto tuple = std::reinterpret_pointer_cast<abstract::AbstractTuple>(cnode->abstract());
    return tuple->elements().size();
  } else {
    return 1;
  }
}

int ConvertInputQuantParam(const PrimitivePtr &prim, bool narrow_range, int32_t numbits) {
  auto quant_param_holder = prim->GetAttr("quant_params")->cast<lite::QuantParamHolderPtr>();
  std::vector<schema::QuantParamT> quants;
  schema::QuantParamT quant_param;
  auto inputMin = prim->GetAttr("input_minq");
  auto inputMax = prim->GetAttr("input_maxq");
  if (inputMin != nullptr && inputMax != nullptr) {
    auto inputMinPtr = inputMin->cast<tensor::TensorPtr>();
    auto inputMaxPtr = inputMax->cast<tensor::TensorPtr>();
    auto *minBuf = static_cast<float *>(inputMinPtr->data_c());
    auto *maxBuf = static_cast<float *>(inputMaxPtr->data_c());
    quant_param.min = *minBuf;
    quant_param.max = *maxBuf;
    auto ret =
      lite::quant::CalQuantizationParams(&quant_param, quant_param.min, quant_param.max, narrow_range, numbits);
    if (ret != RET_OK) {
      MS_LOG(ERROR) << "Can't calculate quant parameters";
      return ret;
    }
    quants.emplace_back(quant_param);
    quant_param_holder->set_input_quant_param(0, quants);
  }

  quants.clear();
  auto filterMin = prim->GetAttr("filter_minq");
  auto filterMax = prim->GetAttr("filter_maxq");
  if (filterMin != nullptr && filterMax != nullptr) {
    auto filterMinPtr = filterMin->cast<tensor::TensorPtr>();
    auto filterMaxPtr = filterMax->cast<tensor::TensorPtr>();
    auto *minBuf = static_cast<float *>(filterMinPtr->data_c());
    auto *maxBuf = static_cast<float *>(filterMaxPtr->data_c());
    quant_param.min = FLT_MAX;
    quant_param.max = FLT_MIN;
    for (int i = 0; i < filterMinPtr->ElementsNum(); ++i) {
      schema::QuantParamT tmp_quant_param;
      tmp_quant_param.min = *minBuf;
      tmp_quant_param.max = *maxBuf;
      auto ret =
        lite::quant::CalQuantizationParams(&tmp_quant_param, tmp_quant_param.min, tmp_quant_param.max, true, numbits);
      if (ret != RET_OK) {
        MS_LOG(ERROR) << "Can't calculate quant parameters";
        return ret;
      }
      quants.emplace_back(tmp_quant_param);
      minBuf++;
      maxBuf++;
    }
    quant_param_holder->set_input_quant_param(1, quants);
  }
  return lite::RET_OK;
}

int ConvertOutputQuantParam(const PrimitivePtr &prim, bool narrow_range, int32_t numbits) {
  auto quant_param_holder = prim->GetAttr("quant_params")->cast<lite::QuantParamHolderPtr>();
  std::vector<schema::QuantParamT> quants;
  schema::QuantParamT quant_param;
  auto outputMin = prim->GetAttr("output_minq");
  auto outputMax = prim->GetAttr("output_maxq");
  if (outputMin != nullptr && outputMax != nullptr) {
    auto outputMinPtr = outputMin->cast<tensor::TensorPtr>();
    auto outputMaxPtr = outputMax->cast<tensor::TensorPtr>();
    auto *minBuf = static_cast<float *>(outputMinPtr->data_c());
    auto *maxBuf = static_cast<float *>(outputMaxPtr->data_c());
    quant_param.min = *minBuf;
    quant_param.max = *maxBuf;
    auto ret =
      lite::quant::CalQuantizationParams(&quant_param, quant_param.min, quant_param.max, narrow_range, numbits);
    if (ret != RET_OK) {
      MS_LOG(ERROR) << "Can't calculate quant parameters";
      return ret;
    }
    quants.emplace_back(quant_param);
    quant_param_holder->set_output_quant_param(0, quants);
  }
  return lite::RET_OK;
}

void CheckQuantParams(const PrimitivePtr &prim) {
  auto quant_param_holder = prim->GetAttr("quant_params")->cast<lite::QuantParamHolderPtr>();
  auto input_quant_params = quant_param_holder->get_input_quant_params();
  bool is_quant = false;
  for (size_t i = 0; i < input_quant_params.size(); ++i) {
    if (!input_quant_params.at(i).empty() && input_quant_params.at(i).at(0).inited) {
      is_quant = true;
      break;
    }
  }
  auto output_quant_params = quant_param_holder->get_output_quant_params();
  for (size_t i = 0; i < output_quant_params.size(); ++i) {
    if (!output_quant_params.at(i).empty() && output_quant_params.at(i).at(0).inited) {
      is_quant = true;
    }
  }
  if (!is_quant) {
    prim->EraseAttr("quant_params");
  }
}

int ConvertQuantParam(const PrimitivePtr &prim, const std::vector<AnfNodePtr> &inputs) {
  auto narrow_range = prim->GetAttr("narrow_range");
  bool narrow_range_param = false;
  if (narrow_range != nullptr) {
    if (utils::isa<tensor::TensorPtr>(narrow_range)) {
      auto narrow_range_tensor = narrow_range->cast<tensor::TensorPtr>();
      narrow_range_param = *reinterpret_cast<bool *>(narrow_range_tensor->data_c());
    } else if (utils::isa<ImmTraits<bool>::type>(narrow_range)) {
      narrow_range_param = GetValue<bool>(narrow_range);
    } else {
      MS_LOG(ERROR) << "valueptr is invalid.";
      return lite::RET_ERROR;
    }
  }
  auto num_bits = prim->GetAttr("num_bits");
  int32_t num_bits_param = 8;
  if (num_bits != nullptr) {
    if (utils::isa<tensor::TensorPtr>(num_bits)) {
      auto num_bits_tensor = num_bits->cast<tensor::TensorPtr>();
      num_bits_param = *reinterpret_cast<int64_t *>(num_bits_tensor->data_c());
    } else if (utils::isa<ImmTraits<int64_t>::type>(num_bits)) {
      num_bits_param = GetValue<int64_t>(num_bits);
    } else {
      MS_LOG(ERROR) << "valueptr is invalid.";
      return lite::RET_ERROR;
    }
  }
  auto status = ConvertInputQuantParam(prim, narrow_range_param, num_bits_param);
  if (status != lite::RET_OK) {
    MS_LOG(ERROR) << "compute int quant param failed.";
    return status;
  }
  status = ConvertOutputQuantParam(prim, narrow_range_param, num_bits_param);
  if (status != lite::RET_OK) {
    MS_LOG(ERROR) << "compute output quant param failed.";
    return status;
  }
  CheckQuantParams(prim);
  return lite::RET_OK;
}
}  // namespace

int MindirAdjustPass::ValueNodeInt64Convert(AnfNodePtr anf_node) {
  if (!utils::isa<ValueNodePtr>(anf_node)) {
    return lite::RET_NO_CHANGE;
  }
  auto valueNode = anf_node->cast<ValueNodePtr>();
  if (valueNode->abstract() == nullptr) {
    return lite::RET_NO_CHANGE;
  }
  auto abstractTensor = utils::cast<abstract::AbstractTensorPtr>(valueNode->abstract());
  if (abstractTensor == nullptr) {
    return lite::RET_NO_CHANGE;
  }
  auto value = abstractTensor->GetValueTrack();
  if (value != nullptr && value->isa<tensor::Tensor>()) {
    if (abstractTensor->element() == nullptr) {
      MS_LOG(ERROR) << "abstractTensor->element() is nullptr.";
      return RET_ERROR;
    }
    auto typePtr = abstractTensor->element()->GetTypeTrack();
    if (typePtr->type_id() == kNumberTypeInt64) {
      auto shape_vector = utils::cast<abstract::ShapePtr>(abstractTensor->BuildShape())->shape();
      auto dest_tensor_info = std::make_shared<tensor::Tensor>(kNumberTypeInt32, shape_vector);
      auto *dest_data_buf = reinterpret_cast<int32_t *>(dest_tensor_info->data_c());
      auto src_tensor_info = value->cast<tensor::TensorPtr>();
      auto *src_data_buf = reinterpret_cast<int64_t *>(src_tensor_info->data_c());
      MS_ASSERT(dest_tensor_info->ElementsNum() == src_tensor_info->ElementsNum());
      for (int i = 0; i < dest_tensor_info->ElementsNum(); i++) {
        dest_data_buf[i] = src_data_buf[i];
      }
      abstractTensor->set_value(dest_tensor_info);
      abstractTensor->set_type(TypeIdToType(kNumberTypeInt32));
      abstractTensor->element()->set_type(TypeIdToType(kNumberTypeInt32));
      valueNode->set_value(dest_tensor_info);
    }
  }
  return lite::RET_NO_CHANGE;
}

int MindirAdjustPass::ComputeQuantParams(std::shared_ptr<AnfNode> anf_node) {
  if (!utils::isa<CNodePtr>(anf_node)) {
    MS_LOG(INFO) << "only cnode need to convert primitive.";
    return lite::RET_NO_CHANGE;
  }
  auto cnode = anf_node->cast<CNodePtr>();
  if (cnode->inputs().empty() || cnode->input(0) == nullptr) {
    MS_LOG(ERROR) << "the cnode is invalid.";
    return lite::RET_NULL_PTR;
  }
  auto value_node = cnode->input(0)->cast<ValueNodePtr>();
  if (value_node == nullptr || value_node->value() == nullptr) {
    MS_LOG(ERROR) << "value node is invalid.";
    return lite::RET_NULL_PTR;
  }
  auto primitive = value_node->value()->cast<PrimitivePtr>();
  if (primitive == nullptr) {
    MS_LOG(ERROR) << "the value is not primitive.";
    return lite::RET_ERROR;
  }
  auto inputs = cnode->inputs();
  inputs.erase(inputs.begin());

  auto quant_param_holder = std::make_shared<lite::QuantParamHolder>(inputs.size(), GetCNodeOutputsSize(anf_node));
  primitive->AddAttr("quant_params", quant_param_holder);

  if (ConvertQuantParam(primitive, inputs) != lite::RET_OK) {
    MS_LOG(ERROR) << "compute quant param failed.";
    return lite::RET_ERROR;
  }
  return lite::RET_OK;
}

bool MindirAdjustPass::Run(const FuncGraphPtr &graph) {
  if (this->fmk_type_ != lite::converter::FmkType_MS) {
    MS_LOG(INFO) << "The framework type of model should be mindir.";
    return lite::RET_OK;
  }
  MS_ASSERT(graph != nullptr);
  graph_ = graph;
  auto node_list = TopoSort(graph->get_return());
  int status = lite::RET_OK;
  bool success_flag = true;
  for (auto &node : node_list) {
    if (utils::isa<CNodePtr>(node)) {
      status = ComputeQuantParams(node);
    } else if (utils::isa<ValueNodePtr>(node)) {
      status = ValueNodeInt64Convert(node);
    }
    if (status != lite::RET_OK && status != lite::RET_NO_CHANGE) {
      lite::ReturnCode::GetSingleReturnCode()->UpdateReturnCode(status);
      success_flag = false;
    }
  }
  if (!success_flag) {
    MS_LOG(ERROR) << "Adjust mindir failed.";
    return false;
  }
  return true;
}
}  // namespace opt
}  // namespace mindspore
