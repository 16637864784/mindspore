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

#include "src/delegate/tensorrt/op/gather_tensorrt.h"
#include "src/delegate/tensorrt/tensorrt_utils.h"

namespace mindspore::lite {
int GatherTensorRT::IsSupport(const schema::Primitive *primitive, const std::vector<mindspore::MSTensor> &in_tensors,
                              const std::vector<mindspore::MSTensor> &out_tensors) {
  if (in_tensors.size() != 3) {
    MS_LOG(ERROR) << "invalid input tensor size: " << in_tensors.size();
    return RET_ERROR;
  }
  if (out_tensors.size() != 1) {
    MS_LOG(ERROR) << "invalid output tensor size: " << out_tensors.size();
    return RET_ERROR;
  }
  if (in_tensors[1].DataType() != DataType::kNumberTypeInt32) {
    MS_LOG(ERROR) << "Gather indices only support Int32";
    return RET_ERROR;
  }
  if (in_tensors[2].ElementNum() == 1) {
    axis_ = static_cast<const int *>(in_tensors[2].Data().get())[0];
  } else {
    MS_LOG(ERROR) << "TensorRT axis is attribute.";
    return RET_ERROR;
  }
  return RET_OK;
}

int GatherTensorRT::AddInnerOp(nvinfer1::INetworkDefinition *network) {
  if (network == nullptr) {
    MS_LOG(ERROR) << "network is invalid";
    return RET_ERROR;
  }
  // convert constant MSTensor to ITensor
  nvinfer1::ITensor *add_tensor = lite::ConvertConstantTensor(network, this->in_tensors_[1]);
  if (add_tensor == nullptr) {
    MS_LOG(ERROR) << "add a new tensor failed for TensorRT GatherTensorRTOp.";
    return RET_ERROR;
  }
  nvinfer1::IGatherLayer *gather_layer =
    network->addGather(*tensorrt_in_tensors_[0], *add_tensor /* indices */, axis_ /* axis */);
  if (gather_layer == nullptr) {
    MS_LOG(ERROR) << "addGather failed for TensorRT.";
    return RET_ERROR;
  }
  gather_layer->setName(op_name_.c_str());
  this->AddInnerOutTensors(gather_layer->getOutput(0));
  return RET_OK;
}
}  // namespace mindspore::lite
