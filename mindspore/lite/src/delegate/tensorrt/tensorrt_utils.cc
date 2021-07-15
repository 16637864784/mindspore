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

#include "src/delegate/tensorrt/tensorrt_utils.h"
#include <map>

namespace mindspore::lite {
nvinfer1::Dims ConvertCudaDims(const std::vector<int64_t> &shape) {
  nvinfer1::Dims dims{};
  if (!shape.empty()) {
    dims.nbDims = shape.size();
    for (int i = 0; i < dims.nbDims; i++) {
      dims.d[i] = shape[i];
    }
  }
  return dims;
}
nvinfer1::Dims ConvertCudaDims(int data, size_t size) {
  nvinfer1::Dims dims{};
  dims.nbDims = size;
  for (size_t i = 0; i < size; i++) {
    dims.d[i] = data;
  }
  return dims;
}

nvinfer1::Dims ConvertCudaDims(void *data, size_t size) {
  nvinfer1::Dims dims{};
  dims.nbDims = size;
  int *dims_data = reinterpret_cast<int *>(data);
  for (size_t i = 0; i < size; i++) {
    dims.d[i] = *(dims_data + i);
  }
  return dims;
}

nvinfer1::IShuffleLayer *SetTranspose(nvinfer1::INetworkDefinition *network, const nvinfer1::ITensor &input,
                                      nvinfer1::Permutation permutation) {
  nvinfer1::IShuffleLayer *layer = network->addShuffle(const_cast<nvinfer1::ITensor &>(input));
  if (layer == nullptr) {
    MS_LOG(ERROR) << "failed to create ShuffleLayer when create transpose op.";
    return nullptr;
  }
  layer->setFirstTranspose(permutation);
  return layer;
}

nvinfer1::DataType ConvertDataType(DataType type_id) {
  std::map<DataType, nvinfer1::DataType> data_type_map = {{DataType::kNumberTypeInt8, nvinfer1::DataType::kINT8},
                                                          {DataType::kNumberTypeInt32, nvinfer1::DataType::kINT32},
                                                          {DataType::kNumberTypeFloat32, nvinfer1::DataType::kFLOAT},
                                                          {DataType::kNumberTypeFloat16, nvinfer1::DataType::kHALF}};
  auto iter = data_type_map.find(type_id);
  nvinfer1::DataType data_type;
  if (iter != data_type_map.end()) {
    data_type = iter->second;
  } else {
    data_type = nvinfer1::DataType::kFLOAT;
    MS_LOG(WARNING) << "invalid data_type for TensorRT, need check";
  }
  return data_type;
}

nvinfer1::IShuffleLayer *NHWC2NCHW(nvinfer1::INetworkDefinition *network, const nvinfer1::ITensor &input) {
  // NHWC 0123 NCHW 0312
  nvinfer1::Permutation perm{{0, 3, 1, 2}};
  return SetTranspose(network, input, perm);
}

nvinfer1::IShuffleLayer *NCHW2NHWC(nvinfer1::INetworkDefinition *network, const nvinfer1::ITensor &input) {
  // NCHW 0123 NHWC 0231
  nvinfer1::Permutation perm{{0, 2, 3, 1}};
  return SetTranspose(network, input, perm);
}

nvinfer1::ITensor *ConvertConstantTensor(nvinfer1::INetworkDefinition *network, mindspore::MSTensor ms_tensor) {
  if (network == nullptr) {
    MS_LOG(ERROR) << "network is null for ConvertConstantTensor";
    return nullptr;
  }
  nvinfer1::Dims dims = ConvertCudaDims(ms_tensor.Shape());
  nvinfer1::DataType data_type = ConvertDataType(ms_tensor.DataType());

  nvinfer1::Weights weights{data_type, ms_tensor.MutableData(), ms_tensor.ElementNum()};
  nvinfer1::IConstantLayer *constant_tensor = network->addConstant(dims, weights);
  if (constant_tensor == nullptr) {
    MS_LOG(ERROR) << "create constant_tensor failed.";
    return nullptr;
  }
  auto name = ms_tensor.Name() + "_constant_layer";
  constant_tensor->setName(name.c_str());
  return constant_tensor->getOutput(0);
}

nvinfer1::ITensor *ConvertScalarToITensor(nvinfer1::INetworkDefinition *network, size_t shape_size, void *value) {
  nvinfer1::Dims dims = ConvertCudaDims(1, shape_size);
  nvinfer1::Weights weights{nvinfer1::DataType::kFLOAT, value, 1};
  nvinfer1::IConstantLayer *constant_tensor = network->addConstant(dims, weights);
  if (constant_tensor == nullptr) {
    MS_LOG(ERROR) << "create constant_tensor failed.";
    return nullptr;
  }
  return constant_tensor->getOutput(0);
}

ActivationParams ConvertActivationType(schema::ActivationType activation_type) {
  std::map<schema::ActivationType, ActivationParams> action_map = {
    {schema::ActivationType_RELU, ActivationParams{nvinfer1::ActivationType::kRELU, false, 0, false, 0}},
    {schema::ActivationType_SIGMOID, ActivationParams{nvinfer1::ActivationType::kSIGMOID, false, 0, false, 0}},
    {schema::ActivationType_TANH, ActivationParams{nvinfer1::ActivationType::kTANH, false, 0, false, 0}},
    {schema::ActivationType_LEAKY_RELU, ActivationParams{nvinfer1::ActivationType::kLEAKY_RELU, true, 0, false, 0}},
    {schema::ActivationType_ELU, ActivationParams{nvinfer1::ActivationType::kELU, true, 0, false, 0}},
    {schema::ActivationType_SELU, ActivationParams{nvinfer1::ActivationType::kSELU, true, 0, true, 0}},
    {schema::ActivationType_SOFTSIGN, ActivationParams{nvinfer1::ActivationType::kSOFTSIGN, false, 0, false, 0}},
    {schema::ActivationType_SOFTPLUS, ActivationParams{nvinfer1::ActivationType::kSOFTPLUS, true, 0, true, 0}},
    {schema::ActivationType_THRESHOLDRELU,
     ActivationParams{nvinfer1::ActivationType::kTHRESHOLDED_RELU, true, 0, false, 0}},
    {schema::ActivationType_RELU6, ActivationParams{nvinfer1::ActivationType::kCLIP, true, 0, true, 6}},
    {schema::ActivationType_RELU1, ActivationParams{nvinfer1::ActivationType::kCLIP, true, 0, true, 1}}};
  auto iter = action_map.find(activation_type);
  ActivationParams action_param = ActivationParams{nvinfer1::ActivationType::kRELU, false, 0, false, 0};
  if (iter != action_map.end()) {
    action_param = iter->second;
  } else {
    MS_LOG(WARNING) << "Unsupported op action type for TensorRT: " << activation_type;
  }
  return action_param;
}

nvinfer1::ITensor *ConvertTensorWithExpandDims(nvinfer1::INetworkDefinition *network, mindspore::MSTensor ms_tensor,
                                               size_t expand_shape_size) {
  if (network == nullptr) {
    MS_LOG(ERROR) << "network is null for ConvertConstantTensor";
    return nullptr;
  }
  std::vector<int64_t> shape(expand_shape_size);
  size_t shape_size = ms_tensor.Shape().size();
  size_t expand_size = expand_shape_size - shape_size;
  for (size_t i = 0; i < expand_shape_size; ++i) {
    if (i < expand_size) {
      shape[i] = 1;
    } else {
      shape[i] = ms_tensor.Shape()[i - expand_size];
    }
  }
  nvinfer1::Dims dims = ConvertCudaDims(shape);
  nvinfer1::DataType data_type = ConvertDataType(ms_tensor.DataType());

  nvinfer1::Weights weights{data_type, ms_tensor.MutableData(), ms_tensor.ElementNum()};
  nvinfer1::IConstantLayer *constant_tensor = network->addConstant(dims, weights);
  if (constant_tensor == nullptr) {
    MS_LOG(ERROR) << "create constant_tensor failed.";
    return nullptr;
  }
  auto name = ms_tensor.Name() + "_constant_layer";
  constant_tensor->setName(name.c_str());
  return constant_tensor->getOutput(0);
}
nvinfer1::Weights TransposeWeight(mindspore::MSTensor ms_tensor, float **pack_weight) {
  // usage notice: malloc addr saved to pack_weight, save pack_weight ptr and free it when deconstruct
  nvinfer1::Weights weights{};
  weights.count = ms_tensor.ElementNum();
  if (lite::ConvertDataType(ms_tensor.DataType()) != nvinfer1::DataType::kFLOAT) {
    MS_LOG(WARNING) << "weights data type is not float";
  }
  weights.type = nvinfer1::DataType::kFLOAT;
  auto weight_shape = ms_tensor.Shape();
  float *src_val = reinterpret_cast<float *>(ms_tensor.MutableData());
  float *pack_weight_tmp = reinterpret_cast<float *>(malloc(ms_tensor.ElementNum() * sizeof(float)));
  if (pack_weight_tmp == nullptr) {
    MS_LOG(ERROR) << "Malloc buffer failed.";
    return weights;
  }
  PackNHWCToNCHWFp32(src_val, pack_weight_tmp, weight_shape[0], weight_shape[1] * weight_shape[2], weight_shape[3], 0,
                     0);
  weights.values = pack_weight_tmp;
  *pack_weight = pack_weight_tmp;
  return weights;
}

nvinfer1::Weights ConvertWeight(mindspore::MSTensor ms_tensor) {
  nvinfer1::Weights weights{};
  weights.type = ConvertDataType(ms_tensor.DataType());
  weights.values = ms_tensor.MutableData();
  weights.count = ms_tensor.ElementNum();
  return weights;
}

}  // namespace mindspore::lite
