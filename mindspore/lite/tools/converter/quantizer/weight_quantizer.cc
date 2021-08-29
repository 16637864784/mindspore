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

#include "tools/converter/quantizer/weight_quantizer.h"
#include <list>
#include <string>
#include <vector>
#include <utility>
#include <unordered_map>
#include "tools/optimizer/common/gllo_utils.h"
#include "tools/converter/preprocess/image_preprocess.h"

using std::string;
using std::vector;

namespace mindspore::lite::quant {
WeightQuantizer::WeightQuantizer(FuncGraphPtr graph, const FullQuantParam &config) : Quantizer(std::move(graph)) {
  quant_strategy_ = std::make_unique<QuantStrategy>(0, 0);
  config_param_ = config;
}

WeightQuantizer::WeightQuantizer(FuncGraphPtr graph, const converter::Flags &config) : Quantizer(std::move(graph)) {
  auto quant_size = config.commonQuantParam.min_quant_weight_size;
  this->bit_num_ = config.commonQuantParam.bit_num;
  if (this->bit_num_ == 0) {
    type_id_ = kNumberTypeInt16;
    this->is_mixed_bit_ = true;
  }
  auto convQuantWeightChannelThreshold = config.commonQuantParam.min_quant_weight_channel;
  quant_strategy_ = std::make_unique<QuantStrategy>(quant_size, convQuantWeightChannelThreshold);
  quant_max_ = (1 << (unsigned int)(this->bit_num_ - 1)) - 1;
  quant_min_ = -(1 << (unsigned int)(this->bit_num_ - 1));
  // parse type_id_
  if (this->bit_num_ > 0 && this->bit_num_ <= kMaxBit) {
    type_id_ = kNumberTypeInt8;
  } else if (this->bit_num_ <= (kMaxBit * 2)) {
    type_id_ = kNumberTypeInt16;
  } else {
    MS_LOG(ERROR) << "invalid input bits";
  }
}

WeightQuantizer::~WeightQuantizer() {
  for (const auto &fp32_output_tensor : fp32_output_tensors_) {
    for (const auto &kv : fp32_output_tensor) {
      delete kv.second;
    }
  }
}

STATUS WeightQuantizer::SetAbstract(const tensor::TensorPtr &tensor_info, const ParameterPtr &param_node,
                                    const PrimitivePtr &primitive) {
  // set dtype
  tensor_info->set_data_type(type_id_);
  auto abstract_base = param_node->abstract();
  if (abstract_base == nullptr) {
    MS_LOG(ERROR) << "Abstract of parameter is nullptr, " << param_node->name();
    return RET_ERROR;
  }
  if (!utils::isa<abstract::AbstractTensorPtr>(abstract_base)) {
    MS_LOG(ERROR) << "Abstract of parameter should be anstract tensor, " << param_node->name();
    return RET_ERROR;
  }
  auto abstract_tensor = utils::cast<abstract::AbstractTensorPtr>(abstract_base);
  abstract_tensor->element()->set_type(TypeIdToType(type_id_));
  auto quant_param_holder = GetCNodeQuantHolder(primitive);
  quant_param_holder->set_quant_type(schema::QuantType_QUANT_WEIGHT);

  weight_quantized_tensors_.insert({tensor_info, param_node});
  return RET_OK;
}

STATUS WeightQuantizer::DoConvQuantize(const CNodePtr &cnode) {
  auto primitive = GetValueNode<PrimitivePtr>(cnode->input(0));
  if (primitive == nullptr) {
    MS_LOG(ERROR) << "primitive is nullptr";
    return RET_ERROR;
  }

  auto input_node = cnode->input(2);
  if (!input_node->isa<Parameter>()) {
    return RET_ERROR;
  }

  ParameterPtr param_node;
  tensor::TensorPtr tensor_info;

  GetLiteParameter(input_node, &param_node, &tensor_info);
  if (param_node == nullptr || tensor_info == nullptr) {
    MS_LOG(ERROR) << "GetLiteParameter error";
    return RET_ERROR;
  }

  if (tensor_info->data_type() != mindspore::kNumberTypeFloat32) {
    MS_LOG(WARNING) << cnode->fullname_with_scope() << " weight data type is not fp32 but " << tensor_info->data_type();
    return RET_OK;
  }
  auto status = RET_ERROR;
  if (is_mixed_bit_) {
    type_id_ = kNumberTypeInt16;
    status = QuantFilter(tensor_info, primitive, QuantType_WeightQuant, WeightQuantType::MIXED_BIT_PER_LAYER, type_id_);
  } else if (type_id_ == kNumberTypeInt8) {
    status = QuantFilter<int8_t>(tensor_info, primitive, QuantType_WeightQuant, quant_max_, quant_min_, bit_num_,
                                 WeightQuantType::FIXED_BIT_PER_CHANNEL, type_id_);
  } else if (type_id_ == kNumberTypeInt16) {
    status = QuantFilter<int16_t>(tensor_info, primitive, QuantType_WeightQuant, quant_max_, quant_min_, bit_num_,
                                  WeightQuantType::FIXED_BIT_PER_CHANNEL, type_id_);
  }
  if (status == RET_CONTINUE) {
    return RET_OK;
  } else if (status != RET_OK) {
    MS_LOG(ERROR) << "QuantFilter failed : " << status;
    return status;
  }
  status = SetAbstract(tensor_info, param_node, primitive);
  if (status != RET_OK) {
    MS_LOG(ERROR) << "SetAbstract failed : " << status;
    return RET_ERROR;
  }
  return RET_OK;
}

STATUS WeightQuantizer::DoMulQuantize(const CNodePtr &cnode) {
  for (size_t i = 1; i < cnode->size(); i++) {
    auto inputNode = cnode->input(i);
    if (inputNode->isa<Parameter>()) {
      auto param_node = inputNode->cast<ParameterPtr>();
      if ((param_node != nullptr) && param_node->has_default()) {
        auto tensor_info = std::static_pointer_cast<tensor::Tensor>(param_node->default_param());
        if ((tensor_info != nullptr) && (tensor_info->data_type() == mindspore::kNumberTypeFloat32) &&
            (tensor_info->Size() > 0) && (tensor_info->data_c() != nullptr)) {
          auto primitive = GetValueNode<PrimitivePtr>(cnode->input(0));
          if (primitive == nullptr) {
            MS_LOG(ERROR) << "primitive is nullptr";
            return RET_ERROR;
          }

          auto status = RET_ERROR;
          auto weight_quant_type = WeightQuantType::FIXED_BIT_PER_CHANNEL;
          if (i == 3) {
            weight_quant_type = WeightQuantType::FIXED_BIT_PER_LAYER;
          }
          if (is_mixed_bit_) {
            status = QuantFilter(tensor_info, primitive, QuantType_WeightQuant, WeightQuantType::MIXED_BIT_PER_LAYER,
                                 type_id_, i - 1);
          } else if (type_id_ == kNumberTypeInt8) {
            status = QuantFilter<int8_t>(tensor_info, primitive, QuantType_WeightQuant, quant_max_, quant_min_,
                                         bit_num_, weight_quant_type, type_id_, i - 1);
          } else if (type_id_ == kNumberTypeInt16) {
            status = QuantFilter<int16_t>(tensor_info, primitive, QuantType_WeightQuant, quant_max_, quant_min_,
                                          bit_num_, weight_quant_type, type_id_, i - 1);
          }
          if (status == RET_CONTINUE) {
            continue;
          } else if (status != RET_OK) {
            MS_LOG(ERROR) << cnode->fullname_with_scope() << " input " << i << " QuantFilter failed : " << status;
            return status;
          }
          status = SetAbstract(tensor_info, param_node, primitive);
          if (status != RET_OK) {
            MS_LOG(ERROR) << cnode->fullname_with_scope() << " input " << i << " SetAbstract failed : " << status;
            return RET_ERROR;
          }
        }
      }
    }
  }
  return RET_OK;
}

STATUS WeightQuantizer::DoLstmQuantize(const CNodePtr &cnode) {
  MS_ASSERT(cnode != nullptr);
  auto op_name = cnode->fullname_with_scope();

  auto primitive = GetValueNode<PrimitivePtr>(cnode->input(0));
  MS_ASSERT(primitive != nullptr);

  if (cnode->inputs().size() < 4) {
    MS_LOG(ERROR) << op_name << " inputs is " << cnode->inputs().size();
    return RET_ERROR;
  }

  auto status = ProcessLstmWeightByIndex(cnode, primitive, 2);
  if (status != RET_OK) {
    MS_LOG(ERROR) << "Process lstm weight i failed.";
    return RET_ERROR;
  }
  status = ProcessLstmWeightByIndex(cnode, primitive, 3);
  if (status != RET_OK) {
    MS_LOG(ERROR) << "Process lstm weight h failed.";
    return RET_ERROR;
  }
  if (cnode->inputs().size() > 4) {
    status = ProcessLstmWeightByIndex(cnode, primitive, 4);
    if (status != RET_OK) {
      MS_LOG(ERROR) << "Process lstm bias failed.";
      return RET_ERROR;
    }
  }

  return status;
}

STATUS WeightQuantizer::DoGatherQuantize(const CNodePtr &cnode) {
  auto primitive = GetValueNode<PrimitivePtr>(cnode->input(0));
  MS_ASSERT(primitive != nullptr);

  auto first_input = cnode->input(1);
  ParameterPtr param_node;
  tensor::TensorPtr tensor_info;
  GetLiteParameter(first_input, &param_node, &tensor_info);
  if (param_node == nullptr || tensor_info == nullptr || tensor_info->data_type() != TypeId::kNumberTypeFloat32) {
    MS_LOG(INFO) << "This Gather op " << cnode->fullname_with_scope() << " can not quant weight";
    return RET_OK;
  }

  if (tensor_info->Size() / sizeof(float) < quant_strategy_->m_weight_size_) {
    MS_LOG(INFO) << cnode->fullname_with_scope() << " param cnt: " << (tensor_info->Size() / sizeof(float)) << " < "
                 << quant_strategy_->m_weight_size_;
    return RET_OK;
  }

  auto status = RET_ERROR;
  if (is_mixed_bit_) {
    status =
      QuantFilter(tensor_info, primitive, QuantType_WeightQuant, WeightQuantType::MIXED_BIT_PER_LAYER, type_id_, 0);
  } else if (type_id_ == kNumberTypeInt8) {
    status = QuantFilter<int8_t>(tensor_info, primitive, QuantType_WeightQuant, quant_max_, quant_min_, bit_num_,
                                 WeightQuantType::FIXED_BIT_PER_LAYER, type_id_, 0);
  } else if (type_id_ == kNumberTypeInt16) {
    status = QuantFilter<int16_t>(tensor_info, primitive, QuantType_WeightQuant, quant_max_, quant_min_, bit_num_,
                                  WeightQuantType::FIXED_BIT_PER_LAYER, type_id_, 0);
  }
  if (status == RET_CONTINUE) {
    return RET_OK;
  } else if (status != RET_OK) {
    MS_LOG(ERROR) << "QuantFilter failed : " << status;
    return status;
  }
  status = SetAbstract(tensor_info, param_node, primitive);
  if (status != RET_OK) {
    MS_LOG(ERROR) << "SetAbstract failed : " << status;
    return RET_ERROR;
  }
  return RET_OK;
}

STATUS WeightQuantizer::DoOptimizerQuantize(const CNodePtr &cnode) {
  auto primitive = GetValueNode<PrimitivePtr>(cnode->input(0));
  MS_ASSERT(primitive != nullptr);

  std::vector<int> weight_indices = {2};
  if (opt::CheckPrimitiveType(cnode, prim::kPrimAdam)) {
    weight_indices = {2, 3};
  }
  if (opt::CheckPrimitiveType(cnode, prim::kPrimSGD)) {
    weight_indices = {4, 6};
  }

  for (int idx : weight_indices) {
    auto input = cnode->input(idx);
    if (!quant_strategy_->CanTensorQuantized(input)) {
      MS_LOG(INFO) << "Input " << idx << "of Optimizer is not quantizable";
      continue;
    }
    ParameterPtr param_node;
    tensor::TensorPtr tensor_info;
    GetLiteParameter(input, &param_node, &tensor_info);
    if (param_node == nullptr || tensor_info == nullptr || tensor_info->data_type() != TypeId::kNumberTypeFloat32) {
      MS_LOG(INFO) << "This Gather op " << cnode->fullname_with_scope() << " can not quant weight";
      return RET_OK;
    }

    auto status = RET_ERROR;
    if (type_id_ == kNumberTypeInt8) {
      status = QuantFilter<int8_t>(tensor_info, primitive, QuantType_WeightQuant, quant_max_, quant_min_, bit_num_,
                                   WeightQuantType::FIXED_BIT_PER_LAYER, type_id_, idx - 1);
    } else if (type_id_ == kNumberTypeInt16) {
      status = QuantFilter<int16_t>(tensor_info, primitive, QuantType_WeightQuant, quant_max_, quant_min_, bit_num_,
                                    WeightQuantType::FIXED_BIT_PER_LAYER, type_id_, idx - 1);
    }
    if (status != RET_OK && status != RET_CONTINUE) {
      MS_LOG(ERROR) << "QuantFilter failed : " << status;
      return status;
    }
    status = SetAbstract(tensor_info, param_node, primitive);
    if (status != RET_OK) {
      MS_LOG(ERROR) << "SetAbstract failed : " << status;
      return RET_ERROR;
    }
  }
  return RET_OK;
}

STATUS WeightQuantizer::DoMarkWeightQuantizeIfQuantized(const CNodePtr &cnode) {
  auto primitive = GetValueNode<PrimitivePtr>(cnode->input(0));
  if (primitive == nullptr) {
    MS_LOG(ERROR) << "primitive is nullptr";
    return RET_ERROR;
  }

  auto quant_param_holder = GetCNodeQuantHolder(primitive);
  if (quant_param_holder->quant_type() == schema::QuantType_QUANT_WEIGHT) {
    // already marked with QUANT_WEIGHT
    return RET_OK;
  }

  for (size_t i = 1; i < cnode->size(); i++) {
    auto inputNode = cnode->input(i);
    if (inputNode->isa<Parameter>()) {
      ParameterPtr param_node;
      tensor::TensorPtr tensor_info;
      GetLiteParameter(inputNode, &param_node, &tensor_info);
      auto param = weight_quantized_tensors_.find(tensor_info);
      if (param != weight_quantized_tensors_.end()) {
        quant_param_holder->set_quant_type(schema::QuantType_QUANT_WEIGHT);
        continue;
      }
    }
  }
  return RET_OK;
}

STATUS WeightQuantizer::ProcessLstmWeightByIndex(const CNodePtr &cnode, const PrimitivePtr &primitive,
                                                 const int &index) {
  auto op_name = cnode->fullname_with_scope();
  auto weight_i = cnode->input(index);
  ParameterPtr param_node;

  tensor::TensorPtr tensor_info;
  GetLiteParameter(weight_i, &param_node, &tensor_info);
  if (param_node == nullptr || tensor_info == nullptr) {
    MS_LOG(INFO) << "LSTM input index " << index << " is not weight";
    return RET_OK;
  }
  if (tensor_info->data_type() != TypeId::kNumberTypeFloat32) {
    MS_LOG(WARNING) << "tensor_info tensor type is: " << tensor_info->data_type() << " not quant";
    return RET_OK;
  }
  if (tensor_info->Size() / sizeof(float) < quant_strategy_->m_weight_size_) {
    MS_LOG(INFO) << op_name << " weight_i cnt: " << (tensor_info->Size() / sizeof(float)) << " < "
                 << quant_strategy_->m_weight_size_;
    return RET_OK;
  }
  auto status = RET_ERROR;
  if (is_mixed_bit_) {
    status = QuantFilter(tensor_info, primitive, QuantType_WeightQuant, WeightQuantType::MIXED_BIT_PER_LAYER, type_id_,
                         index - 1);
  } else if (type_id_ == kNumberTypeInt8) {
    status = QuantFilter<int8_t>(tensor_info, primitive, QuantType_WeightQuant, quant_max_, quant_min_, bit_num_,
                                 WeightQuantType::FIXED_BIT_PER_CHANNEL, type_id_, index - 1);
  } else if (type_id_ == kNumberTypeInt16) {
    status = QuantFilter<int16_t>(tensor_info, primitive, QuantType_WeightQuant, quant_max_, quant_min_, bit_num_,
                                  WeightQuantType::FIXED_BIT_PER_CHANNEL, type_id_, index - 1);
  }
  if (status == RET_CONTINUE) {
    return RET_OK;
  } else if (status != RET_OK) {
    MS_LOG(ERROR) << "QuantFilter failed : " << status;
    return status;
  }
  status = SetAbstract(tensor_info, param_node, primitive);
  if (status != RET_OK) {
    MS_LOG(ERROR) << "SetAbstract failed : " << status;
    return RET_ERROR;
  }
  return RET_OK;
}

constexpr float relative_tolerance = 1e-5;
constexpr float abs_tolerance = 1e-4;

template <typename T>
float CompareOutputData(const std::unordered_map<std::string, mindspore::tensor::MSTensor *> &expected_tensor,
                        const std::unordered_map<std::string, mindspore::tensor::MSTensor *> &compare_tensor) {
  auto valid_data = [](T data) -> bool { return (!std::isnan(data) && !std::isinf(data)); };

  float total_mean_error = 0.0f;
  int tensor_cnt = expected_tensor.size();
  if (tensor_cnt <= 0) {
    MS_LOG(ERROR) << "unexpected tensor_cnt: " << tensor_cnt;
    return RET_ERROR;
  }

  for (const auto &exp_tensor_pair : expected_tensor) {
    float mean_error = 0.0f;
    int error_cnt = 0;

    auto exp_tensor_name = exp_tensor_pair.first;
    auto exp_tensor = exp_tensor_pair.second;
    auto cmp_tensor_find_iter = compare_tensor.find(exp_tensor_name);
    if (cmp_tensor_find_iter == compare_tensor.end()) {
      MS_LOG(ERROR) << "can not find: " << exp_tensor_name;
      return RET_ERROR;
    }
    auto cmp_tensor = cmp_tensor_find_iter->second;

    auto exp_tensor_shape = exp_tensor->shape();
    auto cmp_tensor_shape = cmp_tensor->shape();
    if (exp_tensor_shape != cmp_tensor_shape) {
      MS_LOG(ERROR) << "exp tensor shape not equal to cmp. exp_tensor_elem_cnt: " << exp_tensor->ElementsNum()
                    << " cmp_tensor_elem_cnt: " << cmp_tensor->ElementsNum();
      return RET_ERROR;
    }
    auto exp_data = static_cast<T *>(exp_tensor->MutableData());
    auto cmp_data = static_cast<T *>(cmp_tensor->MutableData());
    auto elem_cnt = exp_tensor->ElementsNum();
    for (int i = 0; i < elem_cnt; i++) {
      if (!valid_data(exp_data[i]) || !valid_data(cmp_data[i])) {
        MS_LOG(ERROR) << "data is not valid. exp: " << exp_data[i] << " cmp: " << cmp_data[i] << " index: " << i;
        return RET_ERROR;
      }
      auto tolerance = abs_tolerance + relative_tolerance * fabs(exp_data[i]);
      auto abs_error = std::fabs(exp_data[i] - cmp_data[i]);
      if (abs_error > tolerance) {
        if (fabs(exp_data[i] == 0)) {
          if (abs_error > 1e-5) {
            mean_error += abs_error;
            error_cnt++;
          } else {
            // it is ok, very close to 0
            continue;
          }
        } else {
          mean_error += abs_error / (fabs(exp_data[i]) + FLT_MIN);
          error_cnt++;
        }
      } else {
        // it is ok, no error
        continue;
      }
    }  // end one tensor data loop
    total_mean_error += mean_error / elem_cnt;
  }  // end tensor loop
  return total_mean_error / tensor_cnt;
}

STATUS WeightQuantizer::RunFp32Graph(const FuncGraphPtr &func_graph) {
  auto image_cnt = images_.at(0).size();
  if (!config_param_.input_shapes.empty()) {
    if (config_param_.input_shapes.size() != image_cnt) {
      MS_LOG(ERROR) << "input_shapes size: " << config_param_.input_shapes.size() << " image_cnt: " << image_cnt;
      return RET_ERROR;
    }
  }
  // 0.1 Create Fp32 Session
  flags.commonQuantParam.quant_type = schema::QuantType_QUANT_NONE;
  auto fp32_sm = CreateSessionByFuncGraph(func_graph, flags, config_param_.thread_num);
  auto fp32_session = fp32_sm.session;
  auto fp32_model = fp32_sm.model;
  if (fp32_session == nullptr || fp32_model == nullptr) {
    MS_LOG(ERROR) << "CreateSessoin fail";
    delete fp32_model;
    return RET_ERROR;
  }
  auto fp32_inputs = fp32_session->GetInputs();
  fp32_output_tensors_.resize(image_cnt);
  // 0.3 save fp32 output
  for (size_t i = 0; i < image_cnt; i++) {
    if (!config_param_.input_shapes.empty()) {
      std::vector<std::vector<int>> shapes;
      auto status = ConvertInputShapeMapToVector(&config_param_, fp32_session->GetInputs(), &shapes);
      if (status != RET_OK) {
        MS_LOG(ERROR) << "Convert input shape map to vector failed.";
        delete fp32_sm.session;
        delete fp32_sm.model;
        return RET_ERROR;
      }
      status = fp32_session->Resize(fp32_inputs, shapes);
      if (status != RET_OK) {
        MS_LOG(ERROR) << "session Resize fail";
        delete fp32_sm.session;
        delete fp32_sm.model;
        return RET_ERROR;
      }
    }
    for (size_t input_index = 0; input_index < fp32_inputs.size(); input_index++) {
      auto status = preprocess::PreProcess(flags.dataPreProcessParam, fp32_inputs[input_index]->tensor_name(), i,
                                           fp32_inputs[input_index]);
      if (status != RET_OK) {
        MS_LOG(ERROR) << "generate input data from images failed!";
        delete fp32_sm.session;
        delete fp32_sm.model;
        return RET_ERROR;
      }
    }
    auto status = fp32_session->RunGraph();
    if (status != RET_OK) {
      MS_LOG(ERROR) << "RunGraph fail";
      delete fp32_sm.session;
      delete fp32_sm.model;
      return RET_ERROR;
    }
    auto fp32_outputs = fp32_session->GetOutputs();
    for (const auto &kv : fp32_outputs) {
      auto *tensor = kv.second;
      auto *lite_tensor = reinterpret_cast<lite::Tensor *>(tensor);
      if (lite_tensor == nullptr) {
        MS_LOG(ERROR) << "not lite tensor";
        delete fp32_sm.session;
        delete fp32_sm.model;
        return RET_ERROR;
      }
      auto *new_tensor = Tensor::CopyTensor(*lite_tensor, true);
      fp32_output_tensors_[i][kv.first] = new_tensor;
    }
  }
  delete fp32_sm.session;
  delete fp32_sm.model;
  return RET_OK;
}

STATUS WeightQuantizer::DoMixedQuantize(const FuncGraphPtr &func_graph) {
  auto cnodes = func_graph->GetOrderedCnodes();
  int status = RET_OK;
  for (auto &cnode : cnodes) {
    if (opt::CheckPrimitiveType(cnode, prim::kPrimLstm)) {
      status = DoLstmQuantize(cnode);
      if (status != RET_OK) {
        MS_LOG(ERROR) << "DoLstmQuantize error";
        return RET_ERROR;
      }
    } else if (opt::CheckPrimitiveType(cnode, prim::kPrimGather)) {
      status = DoGatherQuantize(cnode);
      if (status != RET_OK) {
        MS_LOG(ERROR) << "DoGatherQuantize error";
        return RET_ERROR;
      }
    }
  }
  return status;
}
STATUS WeightQuantizer::CheckImageCnt() {
  auto image_cnt = images_.at(0).size();
  if (!config_param_.input_shapes.empty()) {
    if (config_param_.input_shapes.size() != image_cnt) {
      MS_LOG(ERROR) << "input_shapes size: " << config_param_.input_shapes.size() << " image_cnt: " << image_cnt;
      return RET_ERROR;
    }
  }
  return RET_OK;
}

STATUS WeightQuantizer::GetParamNodeAndValue(const std::shared_ptr<AnfNode> &input_node, const std::string &op_name,
                                             ParameterPtr *param_node, tensor::TensorPtr *tensor_info) {
  if (!input_node->isa<Parameter>()) {
    MS_LOG(WARNING) << op_name << " the second input is not parameter";
    return RET_CONTINUE;
  }
  *param_node = input_node->cast<ParameterPtr>();
  if (!(*param_node)->has_default()) {
    MS_LOG(WARNING) << op_name << " the second input can not convert to parameter";
    return RET_CONTINUE;
  }
  *tensor_info = std::static_pointer_cast<tensor::Tensor>((*param_node)->default_param());
  if (*tensor_info == nullptr) {
    MS_LOG(WARNING) << op_name << " the second input can not convert to parameter";
    return RET_CONTINUE;
  }
  if ((*tensor_info)->data_type() != TypeId::kNumberTypeFloat32) {
    MS_LOG(WARNING) << op_name << " the second input type is not float";
    return RET_CONTINUE;
  }
  return RET_OK;
}
STATUS WeightQuantizer::TryQuant(const int &bit_num_t, const ParameterPtr &param_node,
                                 const tensor::TensorPtr &tensor_info, const PrimitivePtr &primitive) {
  int status;
  type_id_ = TypeId::kNumberTypeInt8;
  int quant_max_t = (1 << (unsigned int)(bit_num_t - 1)) - 1;
  int quant_min_t = -(1 << (unsigned int)(bit_num_t - 1));

  if (type_id_ == TypeId::kNumberTypeInt8) {
    status = QuantFilter<int8_t>(tensor_info, primitive, QuantType::QuantType_WeightQuant, quant_max_t, quant_min_t,
                                 bit_num_t, WeightQuantType::FIXED_BIT_PER_CHANNEL, type_id_);
  } else if (type_id_ == TypeId::kNumberTypeInt16) {
    status = QuantFilter<int16_t>(tensor_info, primitive, QuantType::QuantType_WeightQuant, quant_max_t, quant_min_t,
                                  bit_num_t, WeightQuantType::FIXED_BIT_PER_CHANNEL, type_id_);
  } else {
    MS_LOG(ERROR) << "unexpected type_id_: " << type_id_;
    return RET_ERROR;
  }
  if (status == RET_CONTINUE) {
    return RET_OK;
  } else if (status != RET_OK) {
    MS_LOG(ERROR) << "quant filter failed.";
    return RET_ERROR;
  }
  status = SetAbstract(tensor_info, param_node, primitive);
  if (status != RET_OK) {
    MS_LOG(ERROR) << "SetAbstract failed : " << status;
    return RET_ERROR;
  }
  return status;
}

STATUS WeightQuantizer::EvaluateQuant(const FuncGraphPtr &func_graph, size_t image_cnt, float *mean_error) {
  if (mean_error == nullptr) {
    MS_LOG(ERROR) << "mean_error is nullptr.";
    return RET_ERROR;
  }
  // 2.1 create quant session, get input, output tensor
  flags.commonQuantParam.quant_type = schema::QuantType_WeightQuant;
  auto quant_sm = CreateSessionByFuncGraph(func_graph, flags, config_param_.thread_num);
  auto quant_session = std::unique_ptr<session::LiteSession>(quant_sm.session);
  int status;
  if (quant_session == nullptr) {
    MS_LOG(ERROR) << "create session error.";
    delete quant_sm.model;
    return RET_ERROR;
  }
  auto quant_inputs = quant_session->GetInputs();

  for (size_t i = 0; i < image_cnt; i++) {
    if (!config_param_.input_shapes.empty()) {
      std::vector<std::vector<int>> shapes;
      status = ConvertInputShapeMapToVector(&config_param_, quant_session->GetInputs(), &shapes);
      if (status != RET_OK) {
        MS_LOG(ERROR) << "Convert input shape map to vector failed.";
        delete quant_sm.model;
        return RET_ERROR;
      }
      status = quant_session->Resize(quant_inputs, shapes);
      if (status != RET_OK) {
        MS_LOG(ERROR) << "session Resize fail";
        delete quant_sm.model;
        return RET_ERROR;
      }
    }

    // set multi-input data
    for (size_t input_index = 0; input_index < quant_inputs.size(); input_index++) {
      status = preprocess::PreProcess(flags.dataPreProcessParam, quant_inputs[input_index]->tensor_name(), i,
                                      quant_inputs[input_index]);
      if (status != RET_OK) {
        MS_LOG(ERROR) << "generate input data from images failed!";
        delete quant_sm.model;
        return RET_ERROR;
      }
    }
    status = quant_session->RunGraph();
    if (status != RET_OK) {
      MS_LOG(ERROR) << "quant session run error";
      delete quant_sm.model;
      return RET_ERROR;
    }
    // 3. compare between quant and fp32
    auto quant_outputs = quant_session->GetOutputs();
    (*mean_error) += CompareOutputData<float>(fp32_output_tensors_[i], quant_outputs);
  }  // end_for: calib data loop
  delete quant_sm.model;
  (*mean_error) = (*mean_error) / image_cnt;
  return RET_OK;
}

STATUS WeightQuantizer::DoQuantSearch(const FuncGraphPtr &func_graph) {
  auto cnodes = func_graph->GetOrderedCnodes();
  size_t image_cnt = images_.at(0).size();
  int status = RET_OK;
  for (auto iter = cnodes.end(); iter != cnodes.begin();) {
    auto cnode = *(--iter);
    auto primitive = GetValueNode<PrimitivePtr>(cnode->input(0));
    if (primitive == nullptr) {
      MS_LOG(ERROR) << "primitive is null.";
      return RET_ERROR;
    }
    auto op_name = cnode->fullname_with_scope();
    MS_LOG(DEBUG) << "process node: " << op_name << " type: " << primitive->name();
    if (quant_strategy_->CanConvOpQuantized(cnode) || quant_strategy_->CanMulOpQuantized(cnode)) {
      auto input_node = cnode->input(2);
      ParameterPtr param_node;
      tensor::TensorPtr tensor_info;
      status = GetParamNodeAndValue(input_node, op_name, &param_node, &tensor_info);
      if (status == RET_CONTINUE) {
        continue;
      }
      // copy origin data in case to recover
      auto *raw_data = static_cast<float *>(tensor_info->data_c());
      auto elem_count = tensor_info->DataSize();
      auto val = std::make_unique<float>(elem_count);
      std::unique_ptr<float[]> origin_data(val.release());
      auto ret = memcpy_s(origin_data.get(), sizeof(float) * elem_count, raw_data, tensor_info->Size());
      if (ret != EOK) {
        MS_LOG(ERROR) << "memcpy fail: "
                      << " dst size: " << sizeof(float) * elem_count << " src size: " << tensor_info->Size();
        return RET_ERROR;
      }
      // 1. try quant
      for (size_t bit_num_t = 2; bit_num_t <= kMaxBit; bit_num_t++) {
        status = TryQuant(bit_num_t, param_node, tensor_info, primitive);
        if (status != RET_OK) {
          MS_LOG(ERROR) << "TryQuant failed.";
          return RET_ERROR;
        }
        // 2. evaluate the quant
        float mean_error = 0.0f;
        status = EvaluateQuant(func_graph, image_cnt, &mean_error);
        if (status != RET_OK) {
          MS_LOG(ERROR) << "EvaluateQuant failed.";
          return RET_ERROR;
        }
        if (mean_error <= config_param_.mean_error_threshold) {
          MS_LOG(DEBUG) << "op: " << op_name << " got mixed bit: " << bit_num_t << " mean_error: " << mean_error;
          opname_bit_[op_name] = bit_num_t;
          break;
        } else if (bit_num_t != kMaxBit) {
          MS_LOG(DEBUG) << "op: " << op_name << " intermediate bit: " << bit_num_t << " mean_error: " << mean_error
                        << " [recover]";
          // recover
          status =
            UpdateTensorDataAndSize(tensor_info, origin_data.get(), sizeof(float) * elem_count, kNumberTypeFloat32);
          if (status != RET_OK) {
            MS_LOG(ERROR) << "UpdateTensorDataAndSize fail";
            return RET_ERROR;
          }
        } else {
          MS_LOG(DEBUG) << "op: " << op_name << " set bit: " << bit_num_t << " mean_error: " << mean_error;
          opname_bit_[op_name] = bit_num_t;
        }
      }  // end bit loop
    }    //  if: conv and matmul
  }      // end loop: all cnode
  return status;
}

STATUS WeightQuantizer::DoMixedQuant(const FuncGraphPtr &func_graph) {
  auto status = RunFp32Graph(func_graph);
  if (status != RET_OK) {
    MS_LOG(ERROR) << "RunFp32Graph failed.";
    return RET_ERROR;
  }

  status = DoMixedQuantize(func_graph);
  if (status != RET_OK) {
    MS_LOG(ERROR) << "DoMixedQuantize failed.";
    return RET_ERROR;
  }

  status = CheckImageCnt();
  if (status != RET_OK) {
    MS_LOG(ERROR) << "CheckImageCnt failed.";
    return RET_ERROR;
  }

  status = DoQuantSearch(func_graph);
  if (status != RET_OK) {
    MS_LOG(ERROR) << "DoQuantSearch failed.";
    return RET_ERROR;
  }

  for (const auto &kv : opname_bit_) {
    MS_LOG(INFO) << "op: " << kv.first << " bit:" << kv.second;
  }
  return RET_OK;
}

STATUS WeightQuantizer::DoFixedQuant(const FuncGraphPtr &func_graph) {
  MS_ASSERT(func_graph != nullptr);
  weight_quantized_tensors_.clear();

  for (auto &cnode : func_graph->GetOrderedCnodes()) {
    auto primitive = GetValueNode<std::shared_ptr<ops::PrimitiveC>>(cnode->input(0));
    if (primitive == nullptr) {
      MS_LOG(DEBUG) << cnode->fullname_with_scope() << " : primitive is nullptr";
      continue;
    }
    auto op_name = cnode->fullname_with_scope();

    if (quant_strategy_->CanConvOpQuantized(cnode)) {
      auto status = DoConvQuantize(cnode);
      if (status != RET_OK) {
        MS_LOG(ERROR) << "DoConvQuantize error";
        return RET_ERROR;
      }
    } else if (quant_strategy_->CanMulOpQuantized(cnode)) {
      auto status = DoMulQuantize(cnode);
      if (status != RET_OK) {
        MS_LOG(ERROR) << "DoMulQuantize error";
        return RET_ERROR;
      }
    } else if (opt::CheckPrimitiveType(cnode, prim::kPrimLstm)) {
      auto status = DoLstmQuantize(cnode);
      if (status != RET_OK) {
        MS_LOG(ERROR) << "DoLstmQuantize error";
        return RET_ERROR;
      }
    } else if (opt::CheckPrimitiveType(cnode, prim::kPrimGather)) {
      auto status = DoGatherQuantize(cnode);
      if (status != RET_OK) {
        MS_LOG(ERROR) << "DoGatherQuantize error";
        return RET_ERROR;
      }
    } else if ((opt::CheckPrimitiveType(cnode, prim::kPrimAdam)) || (opt::CheckPrimitiveType(cnode, prim::kPrimSGD)) ||
               (opt::CheckPrimitiveType(cnode, prim::kPrimApplyMomentum))) {
      auto status = DoOptimizerQuantize(cnode);
      if (status != RET_OK) {
        MS_LOG(ERROR) << "DoOptimizerQuantize error";
        return RET_ERROR;
      }
    } else {
      MS_LOG(DEBUG) << op_name << " of type: " << primitive->name() << " no need quant";
    }
  }
  return MarkWeightQuantizationInNodes(func_graph);
}

STATUS WeightQuantizer::MarkWeightQuantizationInNodes(const FuncGraphPtr &func_graph) {
  MS_ASSERT(func_graph != nullptr);
  for (auto &cnode : func_graph->GetOrderedCnodes()) {
    auto primitive = GetValueNode<std::shared_ptr<ops::PrimitiveC>>(cnode->input(0));
    if (primitive == nullptr) {
      MS_LOG(DEBUG) << cnode->fullname_with_scope() << " : primitive is nullptr";
      continue;
    }
    auto status = DoMarkWeightQuantizeIfQuantized(cnode);
    if (status != RET_OK) {
      MS_LOG(ERROR) << "MarkWeightQuantizationInNodes error marking " << cnode->fullname_with_scope();
      return RET_ERROR;
    }
  }
  return RET_OK;
}

STATUS WeightQuantizer::DoQuantize(FuncGraphPtr func_graph) {
  MS_ASSERT(func_graph != nullptr);
  if (config_param_.mixed) {
    bit_num_ = kMaxBit;
    quant_max_ = (1 << (unsigned int)(this->bit_num_ - 1)) - 1;
    quant_min_ = -(1 << (unsigned int)(this->bit_num_ - 1));
    type_id_ = kNumberTypeInt8;
    MS_LOG(INFO) << "Do mixed bit quantization";
    return DoMixedQuant(func_graph);
  }

  return DoFixedQuant(func_graph);
}
}  // namespace mindspore::lite::quant
