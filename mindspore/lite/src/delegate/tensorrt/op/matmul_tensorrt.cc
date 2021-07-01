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

#include "src/delegate/tensorrt/op/matmul_tensorrt.h"
#include "src/delegate/tensorrt/tensorrt_utils.h"

namespace mindspore::lite {
int mindspore::lite::MatMulTensorRT::IsSupport(const mindspore::schema::Primitive *primitive,
                                               const std::vector<tensor::MSTensor *> &in_tensors,
                                               const std::vector<tensor::MSTensor *> &out_tensors) {
  if (in_tensors.size() != 2 && in_tensors.size() != 3) {
    MS_LOG(ERROR) << "Unsupported input tensor size, size is " << in_tensors.size();
    return RET_ERROR;
  }
  if (out_tensors.size() != 1) {
    MS_LOG(ERROR) << "Unsupported output tensor size, size is " << out_tensors.size();
    return RET_ERROR;
  }
  return RET_OK;
}

int mindspore::lite::MatMulTensorRT::AddInnerOp(nvinfer1::INetworkDefinition *network) {
  auto primitive = this->GetPrimitive()->value_as_MatMul();
  transpose_a_ = primitive->transpose_a() ? nvinfer1::MatrixOperation::kTRANSPOSE : nvinfer1::MatrixOperation::kNONE;
  transpose_b_ = primitive->transpose_b() ? nvinfer1::MatrixOperation::kTRANSPOSE : nvinfer1::MatrixOperation::kNONE;
  auto weight = ConvertTensorWithExpandDims(network, in_tensors_[1], in_tensors_[0]->shape().size());

  auto matmul_layer = network->addMatrixMultiply(*tensorrt_in_tensors_[0], transpose_a_, *weight, transpose_b_);
  matmul_layer->setName(op_name_.c_str());

  if (in_tensors_.size() == 3) {
    auto bias = ConvertTensorWithExpandDims(network, in_tensors_[2], in_tensors_[0]->shape().size());
    auto bias_layer = network->addElementWise(*matmul_layer->getOutput(0), *bias, nvinfer1::ElementWiseOperation::kSUM);
    auto bias_layer_name = op_name_ + "_bias";
    bias_layer->setName(bias_layer_name.c_str());
    this->AddInnerOutTensors(bias_layer->getOutput(0));
  } else {
    this->AddInnerOutTensors(matmul_layer->getOutput(0));
  }
  return RET_OK;
}
}  // namespace mindspore::lite
