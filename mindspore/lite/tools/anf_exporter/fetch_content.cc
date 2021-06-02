/**
 * Copyright 2021 Huawei Technologies Co., Ltd
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

#include "tools/anf_exporter/fetch_content.h"
#include <algorithm>
#include <string>
#include <vector>
#include <unordered_map>
#include "tools/converter/quant_param_holder.h"
#include "tools/optimizer/common/gllo_utils.h"
#include "utils/check_convert_utils.h"

namespace mindspore {
namespace lite {
namespace {
constexpr size_t kTensorListMinSize = 3 * sizeof(int32_t);
static const std::unordered_map<int, int> TypeToTypeMap = {
  {kNumberTypeInt, kNumberTypeInt32}, {kNumberTypeUInt, kNumberTypeUInt32}, {kNumberTypeFloat, kNumberTypeFloat32}};
STATUS GetShapeVectorFromStringTensor(const tensor::TensorPtr &tensor_info, ShapeVector *shape_vector, size_t *offset) {
  MS_ASSERT(tensor_info != nullptr && shape_vector != nullptr && offset != nullptr);
  auto data_type = tensor_info->data_type();
  if (data_type != kObjectTypeString) {
    MS_LOG(ERROR) << "This function only used for string tensor.";
    return RET_ERROR;
  }
  shape_vector->clear();
  auto tensor_data = reinterpret_cast<uint8_t *>(tensor_info->data_c());
  std::string shape_str;
  std::string shape_size_str;
  *offset = 0;
  size_t cnt = 0;
  for (; *offset < tensor_info->Size(); (*offset)++) {
    if (tensor_data[*offset] == ',') {
      (*offset)++;
      break;
    }
    shape_size_str.push_back(tensor_data[*offset]);
  }
  if (*offset == 0) {
    MS_LOG(ERROR) << "string tensor's dim size not found.";
    return RET_ERROR;
  }
  size_t shape_size = std::stoi(shape_size_str);
  for (; *offset < tensor_info->Size(); (*offset)++) {
    if (tensor_data[*offset] == ',') {
      cnt++;
      shape_vector->push_back(std::stoi(shape_str));
      shape_str.clear();
    } else {
      shape_str.push_back(tensor_data[*offset]);
    }
    if (cnt == shape_size) {
      (*offset)++;
      break;
    }
  }
  if (shape_vector->empty()) {
    MS_LOG(ERROR) << "string tensor's shape shouldn't be empty.";
    return RET_ERROR;
  }
  return RET_OK;
}
int GetFormatByFmk(int32_t fmk_type) {
  switch (fmk_type) {
    case converter::FmkType_ONNX:
    case lite::converter::FmkType_CAFFE:
    case lite::converter::FmkType_MS:
      return mindspore::NCHW;
    case lite::converter::FmkType_TF:
    case lite::converter::FmkType_TFLITE:
      return mindspore::NHWC;
    default:
      return -1;
  }
}

STATUS GetDataTypeAndShape(const ParameterPtr &param_node, TypeId *data_type, ShapeVector *shape_vector) {
  MS_ASSERT(param_node != nullptr && data_type != nullptr && shape_vector != nullptr);
  auto abstract_base = param_node->abstract();
  if (abstract_base == nullptr) {
    MS_LOG(ERROR) << "Abstract of parameter is nullptr, " << param_node->name();
    return RET_PARAM_INVALID;
  }
  if (!utils::isa<abstract::AbstractTensorPtr>(abstract_base)) {
    MS_LOG(ERROR) << "Abstract of parameter should be anstract tensor, " << param_node->name();
    return RET_INPUT_TENSOR_ERROR;
  }
  auto abstract_tensor = utils::cast<abstract::AbstractTensorPtr>(abstract_base);
  auto typePtr = abstract_tensor->element()->GetTypeTrack();
  MS_ASSERT(typePtr != nullptr);
  *data_type = typePtr->type_id();
  if (!utils::isa<abstract::ShapePtr>(abstract_tensor->BuildShape())) {
    MS_LOG(ERROR) << "Shape of Abstract of parameter should be ShapePtr, " << param_node->name();
    return RET_PARAM_INVALID;
  }
  *shape_vector = utils::cast<abstract::ShapePtr>(abstract_tensor->BuildShape())->shape();
  return RET_OK;
}

int FetchFromDefaultParam(const ParameterPtr &param_node, DataInfo *data_info) {
  MS_ASSERT(param_node != nullptr && data_info != nullptr);
  ShapeVector shape_vector;
  TypeId data_type;
  auto status = GetDataTypeAndShape(param_node, &data_type, &shape_vector);
  if (status != RET_OK) {
    MS_LOG(ERROR) << "get data type and shape from param node failed.";
    return RET_ERROR;
  }
  data_info->data_type_ = data_type;
  auto tensor_info = std::dynamic_pointer_cast<tensor::Tensor>(param_node->default_param());
  size_t offset = 0;
  if (!shape_vector.empty() && data_type == kObjectTypeString) {
    status = GetShapeVectorFromStringTensor(tensor_info, &shape_vector, &offset);
    if (status != RET_OK) {
      MS_LOG(ERROR) << "get shape vector from string tensor failed.";
      return RET_ERROR;
    }
  }
  std::vector<int32_t> dims(shape_vector.begin(), shape_vector.end());
  data_info->shape_ = dims;
  if (tensor_info != nullptr && tensor_info->Size() != 0) {
    if (data_type != kObjectTypeTensorType || tensor_info->Size() >= kTensorListMinSize) {
      data_info->data_.resize(tensor_info->Size() - offset);
      if (EOK != memcpy_s(data_info->data_.data(), data_info->data_.size(),
                          static_cast<uint8_t *>(tensor_info->data_c()) + offset, tensor_info->Size() - offset)) {
        MS_LOG(ERROR) << "memcpy_s failed.";
        return RET_ERROR;
      }
    }
  }
  return RET_OK;
}

int FetchFromTensorValue(const ValueNodePtr &value_node, const PrimitivePtr &primitive, converter::FmkType fmk_type,
                         bool train_flag, DataInfo *data_info) {
  MS_ASSERT(value_node != nullptr && primitive != nullptr && data_info != nullptr);
  auto valueAbstract = value_node->abstract();
  auto abstract_tensor = utils::cast<abstract::AbstractTensorPtr>(valueAbstract);
  if (abstract_tensor == nullptr || abstract_tensor->element() == nullptr) {
    MS_LOG(ERROR) << "abstract_tensor or abstract_tensor->element() is nullptr";
    return RET_ERROR;
  }
  auto typePtr = abstract_tensor->element()->GetTypeTrack();
  data_info->data_type_ = typePtr->type_id();
  auto shape_vector = utils::cast<abstract::ShapePtr>(abstract_tensor->BuildShape())->shape();
  std::vector<int32_t> dims(shape_vector.begin(), shape_vector.end());
  data_info->shape_ = dims;
  if (train_flag && dims.empty()) {
    data_info->shape_ = {1};
  }
  auto value = value_node->value();
  MS_ASSERT(value != nullptr);
  auto data = value->cast<tensor::TensorPtr>();
  data_info->data_.resize(data->Size());
  data_info->format_ = GetFormatByFmk(fmk_type);
  if (data_info->format_ != mindspore::NHWC && data_info->format_ != mindspore::NCHW) {
    MS_LOG(ERROR) << "schema tensor format is wrong, " << data_info->format_;
    return RET_ERROR;
  }

  // process weight tensor
  if (data->Size() > 0 && memcpy_s(data_info->data_.data(), data->Size(), data->data_c(), data->Size()) != EOK) {
    MS_LOG(ERROR) << "memcpy_s error.";
    return RET_ERROR;
  }
  return RET_OK;
}

int FetchFromInt32OrInt64ImmValue(const ValueNodePtr &value_node, const PrimitivePtr &primitive, DataInfo *data_info) {
  MS_ASSERT(value_node != nullptr && primitive != nullptr && data_info != nullptr);
  // data of int64 is converted to int32 here.
  data_info->data_type_ = kNumberTypeInt32;
  data_info->shape_ = {1};
  data_info->data_.resize(sizeof(int32_t));
  auto value = value_node->value();
  MS_ASSERT(value != nullptr);
  int real_data = opt::CastToInt(value).front();
  if (memcpy_s(data_info->data_.data(), sizeof(int32_t), &real_data, sizeof(int32_t)) != EOK) {
    MS_LOG(ERROR) << "memcpy_s failed";
    return RET_MEMORY_FAILED;
  }
  return RET_OK;
}

int FetchFromBoolImmValue(const ValueNodePtr &value_node, const PrimitivePtr &primitive, DataInfo *data_info) {
  MS_ASSERT(value_node != nullptr && primitive != nullptr && data_info != nullptr);
  data_info->data_type_ = kNumberTypeBool;
  data_info->shape_ = {1};
  data_info->data_.resize(sizeof(bool));
  auto value = value_node->value();
  MS_ASSERT(value != nullptr);
  auto data = value->cast<mindspore::BoolImmPtr>();
  auto data_value = data->value();
  if (memcpy_s(data_info->data_.data(), sizeof(bool), &data_value, sizeof(bool)) != EOK) {
    MS_LOG(ERROR) << "memcpy_s failed";
    return RET_MEMORY_FAILED;
  }
  return RET_OK;
}

int FetchFromNumberValue(const ValueNodePtr &value_node, const PrimitivePtr &primitive, DataInfo *data_info) {
  MS_ASSERT(value_node != nullptr && primitive != nullptr && data_info != nullptr);
  data_info->data_type_ = kNumberTypeInt32;
  data_info->shape_ = {1};
  data_info->data_.resize(sizeof(int));
  auto data = value_node->value()->cast<NumberPtr>();
  int number_type = data->number_type();
  if (TypeToTypeMap.find(number_type) != TypeToTypeMap.end()) {
    number_type = TypeToTypeMap.at(number_type);
  }
  if (memcpy_s(data_info->data_.data(), sizeof(int), &number_type, sizeof(int)) != EOK) {
    MS_LOG(ERROR) << "memcpy_s failed";
    return RET_MEMORY_FAILED;
  }
  return RET_OK;
}

int FetchFromSequenceValue(const ValueNodePtr &value_node, const PrimitivePtr &primitive, DataInfo *data_info) {
  MS_ASSERT(value_node != nullptr && primitive != nullptr && data_info != nullptr);
  auto value = value_node->value();
  MS_ASSERT(value != nullptr);
  std::vector<int32_t> shape;
  auto value_seq = value->cast<ValueSequeuePtr>();
  MS_ASSERT(value_seq != nullptr);
  if (!value_seq->value().empty()) {
    if (value_seq->value().front()->type()->number_type() == kNumberTypeInt32 ||
        value_seq->value().front()->type()->number_type() == kNumberTypeInt) {
      shape = GetValue<std::vector<int>>(value);
    } else if (value_seq->value().front()->type()->number_type() == kNumberTypeInt64) {
      auto origin_value = GetValue<std::vector<int64_t>>(value);
      std::transform(origin_value.begin(), origin_value.end(), std::back_inserter(shape),
                     [](int64_t val) { return static_cast<int32_t>(val); });
    } else {
      MS_LOG(ERROR) << "Value type is ValueSequence is not integer.";
      return RET_ERROR;
    }
  }
  data_info->data_type_ = kNumberTypeInt32;
  data_info->shape_ = {static_cast<int32_t>(shape.size())};
  data_info->data_.resize(shape.size() * sizeof(int));
  if (!shape.empty() && memcpy_s(data_info->data_.data(), shape.size() * sizeof(int32_t), shape.data(),
                                 shape.size() * sizeof(int32_t)) != EOK) {
    MS_LOG(ERROR) << "memcpy_s data into schema_tensor failed.";
    return RET_ERROR;
  }
  return RET_OK;
}
}  // namespace

int FetchDataFromParameterNode(const CNodePtr &cnode, size_t index, converter::FmkType fmk_type, bool train_flag,
                               DataInfo *data_info) {
  MS_ASSERT(cnode != nullptr && data_info != nullptr);
  auto param_node = cnode->input(index)->cast<ParameterPtr>();
  if (param_node == nullptr) {
    MS_LOG(ERROR) << "input node is not parameter node.";
    return RET_ERROR;
  }
  data_info->format_ = GetFormatByFmk(fmk_type);
  if (data_info->format_ < 0) {
    MS_LOG(ERROR) << "don't support current fmk: " << fmk_type;
    return RET_ERROR;
  }
  if (data_info->format_ != mindspore::NHWC && data_info->format_ != mindspore::NCHW) {
    MS_LOG(ERROR) << "schema tensor format is wrong, " << data_info->format_;
    return RET_ERROR;
  }

  // attr weightFormat is only used by conv-like ops' second input
  auto prim = GetValueNode<PrimitivePtr>(cnode->input(0));
  if ((opt::CheckPrimitiveType(cnode, prim::kPrimConv2DFusion) ||
       opt::CheckPrimitiveType(cnode, opt::kPrimConv2DBackpropInputFusion) ||
       opt::CheckPrimitiveType(cnode, prim::kPrimConv2dTransposeFusion)) &&
      (index == 2 && prim->GetAttr(ops::kFormat) != nullptr)) {
    data_info->format_ = mindspore::KHWC;
  }
  if (FetchFromDefaultParam(param_node, data_info) != RET_OK) {
    MS_LOG(ERROR) << "fetch information from default param failed.";
    return RET_ERROR;
  }
  QuantParamHolderPtr quant_param_holder =
    prim->GetAttr("quant_params") == nullptr ? nullptr : prim->GetAttr("quant_params")->cast<QuantParamHolderPtr>();
  if (quant_param_holder != nullptr && quant_param_holder->enable_huffman_code() &&
      data_info->data_type_ == kNumberTypeInt8) {
    data_info->enable_huffman_code_ = true;
  }
  data_info->node_type_ = NodeType_ValueNode;
  return RET_OK;
}

int FetchDataFromValueNode(const CNodePtr &cnode, size_t index, converter::FmkType fmk_type, bool train_flag,
                           DataInfo *data_info) {
  MS_ASSERT(cnode != nullptr && data_info != nullptr);
  auto value_node = cnode->input(index)->cast<ValueNodePtr>();
  if (value_node == nullptr) {
    MS_LOG(ERROR) << "input node is not value node.";
    return RET_ERROR;
  }
  auto value = value_node->value();
  int ret = RET_OK;
  auto prim = GetValueNode<PrimitivePtr>(cnode->input(0));
  MS_ASSERT(prim != nullptr);
  if (value->isa<tensor::Tensor>()) {
    ret = FetchFromTensorValue(value_node, prim, fmk_type, train_flag, data_info);
    if (index == 2 && prim->GetAttr(ops::kFormat) != nullptr) {
      data_info->format_ = GetValue<int64_t>(prim->GetAttr(ops::kFormat));
    }
  } else if (value->isa<mindspore::Int32Imm>() || value->isa<mindspore::Int64Imm>()) {
    ret = FetchFromInt32OrInt64ImmValue(value_node, prim, data_info);
  } else if (value->isa<mindspore::BoolImm>()) {
    ret = FetchFromBoolImmValue(value_node, prim, data_info);
  } else if (value->isa<mindspore::ValueSequeue>()) {
    ret = FetchFromSequenceValue(value_node, prim, data_info);
  } else if (value->isa<Number>()) {
    ret = FetchFromNumberValue(value_node, prim, data_info);
  } else if (value->isa<FuncGraph>()) {
    MS_LOG(INFO) << "op name:" << value_node->fullname_with_scope() << " input is func_graph";
    return RET_NO_CHANGE;
  } else if (value->isa<Monad>()) {
    MS_LOG(INFO) << "op name:" << value_node->fullname_with_scope() << " input is Monad";
    return RET_NO_CHANGE;
  } else {
    MS_LOG(ERROR) << "Not support value type , need add support.";
    return RET_ERROR;
  }
  data_info->node_type_ = NodeType_ValueNode;
  return ret;
}

int FetchDataFromCNode(const CNodePtr &cnode, size_t index, converter::FmkType fmk_type, bool train_flag,
                       DataInfo *data_info) {
  MS_ASSERT(cnode != nullptr && data_info != nullptr);
  auto abstract = opt::GetCNodeInputAbstract(cnode, index);
  if (abstract == nullptr) {
    MS_LOG(ERROR) << "Abstract cnode is nullptr.";
    return RET_ERROR;
  }
  if (!utils::isa<abstract::AbstractTensorPtr>(abstract)) {
    MS_LOG(ERROR) << "Abstract should be anstract tensor.";
    return RET_ERROR;
  }
  auto abstract_tensor = utils::cast<abstract::AbstractTensorPtr>(abstract);
  auto type_ptr = abstract_tensor->element()->GetTypeTrack();
  MS_ASSERT(typePtr != nullptr);
  if (!utils::isa<abstract::ShapePtr>(abstract_tensor->BuildShape())) {
    MS_LOG(ERROR) << "Shape of Abstract should be ShapePtr.";
    return RET_ERROR;
  }
  auto shape_vector = utils::cast<abstract::ShapePtr>(abstract_tensor->BuildShape())->shape();
  std::vector<int32_t> dims(shape_vector.begin(), shape_vector.end());
  data_info->format_ = mindspore::NHWC;
  data_info->data_type_ = type_ptr->type_id();
  data_info->shape_ = dims;
  data_info->node_type_ = NodeType_CNode;
  if (type_ptr->type_id() == kObjectTypeTensorType) {
    auto tensor_info = abstract_tensor->GetValueTrack();
    if (tensor_info == nullptr || !utils::isa<tensor::TensorPtr>(tensor_info)) {
      MS_LOG(ERROR) << "tensor info is invalid.";
      return RET_ERROR;
    }
    auto tensor_value = tensor_info->cast<tensor::TensorPtr>();
    if (tensor_value->Size() >= kTensorListMinSize) {
      data_info->data_.resize(tensor_value->Size());
      if (memcpy_s(data_info->data_.data(), tensor_value->Size(), tensor_value->data_c(), tensor_value->Size()) !=
          EOK) {
        MS_LOG(ERROR) << "memcpy data failed.";
        return RET_ERROR;
      }
    }
  }
  return RET_OK;
}

void RemoveIfDepend(const CNodePtr &cnode) {
  bool has_depend = false;
  std::vector<AnfNodePtr> inputs;
  inputs.clear();

  inputs.emplace_back(cnode->input(0));
  for (size_t i = 1; i < cnode->inputs().size(); ++i) {
    AnfNodePtr inputNode = cnode->input(i);
    if (!inputNode->isa<CNode>()) {
      inputs.emplace_back(cnode->input(i));
      continue;
    }
    auto depend_node = utils::cast<CNodePtr>(inputNode);
    auto value_node = depend_node->input(0)->cast<ValueNodePtr>();
    if (value_node == nullptr) {
      MS_LOG(ERROR) << "value node is invalid.";
      return;
    }
    if (value_node->value() != nullptr && opt::CheckPrimitiveType(depend_node, prim::kPrimDepend)) {
      has_depend = true;
      bool mask_out = (depend_node->inputs().size() == 3);
      for (size_t j = 1; j < depend_node->inputs().size(); ++j) {
        AnfNodePtr depend_input_node = depend_node->input(j);
        if (depend_input_node->isa<CNode>()) {
          inputs.emplace_back(depend_input_node);
          if (mask_out) {
            break;
          }
        }
      }
    } else {
      inputs.emplace_back(cnode->input(i));
    }
  }
  if (has_depend) {
    cnode->set_inputs(inputs);
  }
}

void RemoveIfMakeTuple(const CNodePtr &cnode) {
  bool has_make_tuple = false;
  std::vector<AnfNodePtr> inputs;
  inputs.clear();

  inputs.emplace_back(cnode->input(0));
  for (size_t i = 1; i < cnode->inputs().size(); ++i) {
    AnfNodePtr input_node = cnode->input(i);
    if (!input_node->isa<CNode>()) {
      inputs.emplace_back(cnode->input(i));
      continue;
    }
    auto make_tuple_node = utils::cast<CNodePtr>(input_node);
    auto value_node = make_tuple_node->input(0)->cast<ValueNodePtr>();
    if (value_node == nullptr) {
      MS_LOG(ERROR) << "value node is invalid.";
      return;
    }
    if (value_node->value() != nullptr && (opt::CheckPrimitiveType(make_tuple_node, prim::kPrimMakeTuple) ||
                                           opt::CheckPrimitiveType(make_tuple_node, opt::kPrimMakeTupleV2))) {
      has_make_tuple = true;
      for (size_t j = 1; j < make_tuple_node->inputs().size(); ++j) {
        inputs.emplace_back(make_tuple_node->input(j));
      }
    } else {
      inputs.emplace_back(cnode->input(i));
    }
  }
  if (has_make_tuple) {
    cnode->set_inputs(inputs);
  }
}
}  // namespace lite
}  // namespace mindspore
