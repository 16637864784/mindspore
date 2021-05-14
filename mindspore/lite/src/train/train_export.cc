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
#define _STUB
#include "src/train/train_export.h"
#include <unistd.h>
#include <sys/stat.h>
#include <fstream>
#include <utility>
#include <map>
#include <set>
#include "schema/inner/model_generated.h"
#include "src/train/train_utils.h"
#include "src/common/quant_utils.h"
#include "tools/common/storage.h"

namespace mindspore {
namespace lite {

std::vector<uint8_t> TrainExport::CreateData(const lite::Tensor *tensor) {
  uint8_t *tensor_data = reinterpret_cast<uint8_t *>(tensor->data_c());
  auto size = tensor->Size();
  std::vector<uint8_t> data(tensor_data, tensor_data + size);
  return data;
}

bool TrainExport::NeedQuantization(const lite::Tensor *tensor) {
  return (tensor->quant_params().size() > 0 && tensor->quant_params().at(0).inited);
}

schema::QuantType TrainExport::GetNodeQuantType(const kernel::LiteKernel *kernel) {
  if (std::any_of(kernel->in_tensors().cbegin(), kernel->in_tensors().cend(), [](const lite::Tensor *t) {
        return (t->IsConst() && (t->quant_params().size() > 0) && (t->quant_params().at(0).inited));
      })) {
    return schema::QuantType_QUANT_WEIGHT;
  }
  return schema::QuantType_QUANT_NONE;
}

int TrainExport::QuantTensorData(schema::TensorT *dest_tensor, const lite::Tensor *src_tensor) {
  int channels = src_tensor->quant_params().size();
  if (channels < 1) {
    MS_LOG(ERROR) << "Quant Params is empty";
    return RET_ERROR;
  }
  int bit_num = src_tensor->quant_params().at(0).bitNum;
  int quant_max = QuantMax(bit_num, kNumberTypeInt8);
  int quant_min = QuantMin(bit_num, kNumberTypeInt8);
  std::vector<int8_t> data(src_tensor->ElementsNum());
  std::vector<schema::QuantParamT> quant_params;

  STATUS ret = RET_OK;
  if (channels == kPerTensor) {
    ret = DoPerLayerQuant<int8_t>(reinterpret_cast<float *>(src_tensor->data_c()), src_tensor->ElementsNum(),
                                  &(quant_params), quant_max, quant_min, bit_num, false, &data);
  } else {
    bool channel_at_first = (src_tensor->shape().at(0) == channels);
    ret = DoPerChannelQuant<int8_t>(reinterpret_cast<float *>(src_tensor->data_c()), src_tensor->ElementsNum(),
                                    schema::QuantType_WeightQuant, &(quant_params), quant_max, quant_min, bit_num,
                                    false, &data, channels, channel_at_first);
  }
  if (ret == RET_QUANT_CONTINUE) {
    MS_LOG(DEBUG) << "No Need to quant per channel";
    return RET_OK;
  }
  if (ret == RET_ERROR) {
    MS_LOG(ERROR) << "QuantTensorData error,  channels = " << channels;
    return ret;
  }
  if (quant_params.empty()) {
    MS_LOG(ERROR) << "quant_params empty";
    return RET_ERROR;
  }
  dest_tensor->data = std::vector<uint8_t>(data.data(), data.data() + data.size());
  dest_tensor->dataType = kNumberTypeInt8;
  dest_tensor->quantParams.clear();
  for (auto quant_param : quant_params) {
    dest_tensor->quantParams.emplace_back(std::make_unique<schema::QuantParamT>(quant_param));
  }

  return RET_OK;
}

std::unique_ptr<schema::TensorT> TrainExport::CreateTensor(const mindspore::lite::Tensor *tensor,
                                                           schema::Tensor *scTensor) {
  auto tensorT = std::make_unique<schema::TensorT>();
  tensorT->nodeType = scTensor->nodeType();
  tensorT->dims = tensor->shape();
  tensorT->format = tensor->format();
  tensorT->name = tensor->tensor_name();
  tensorT->refCount = 0;
  tensorT->offset = 0;
  tensorT->dataType = tensor->data_type();
  tensorT->enableHuffmanCode = false;
  if ((tensorT->nodeType == NodeType_ValueNode) && (scTensor->data() != nullptr) && (scTensor->data()->size() > 0)) {
    if (NeedQuantization(tensor)) {
      QuantTensorData(tensorT.get(), tensor);
    } else {
      tensorT->data = CreateData(tensor);
    }
  }
  tensorT->quantClusters = tensor->quant_clusters();
  return tensorT;
}

Model::Node *TrainExport::FindNode(const mindspore::kernel::LiteKernel *kernel, const Model *model) {
  auto nodes = model->all_nodes_;
  auto it = std::find_if(nodes.begin(), nodes.end(),
                         [&kernel](mindspore::lite::Model::Node *n) { return (kernel->name() == n->name_); });
  if (it == nodes.end()) {
    return nullptr;
  }
  return *it;
}

std::unique_ptr<schema::CNodeT> TrainExport::CreateCNode(const mindspore::kernel::LiteKernel *kernel,
                                                         std::vector<uint32_t> inputIndex,
                                                         std::vector<uint32_t> outputIndex, const Model *model) {
  auto cnodeT = std::make_unique<schema::CNodeT>();
  if (cnodeT == nullptr) {
    MS_LOG(ERROR) << " cannot allocate node";
    return nullptr;
  }
  cnodeT->inputIndex = inputIndex;
  cnodeT->outputIndex = outputIndex;
  cnodeT->name = kernel->name();
  cnodeT->quantType = GetNodeQuantType(kernel);
  // find kernel in model
  auto *node = FindNode(kernel, model);
  if (node == nullptr) {
    MS_LOG(ERROR) << "cannot find kernel " + kernel->name() + " in model";
    return nullptr;
  }
  auto primitive = reinterpret_cast<schema::Primitive *>(const_cast<void *>(node->primitive_));
  cnodeT->primitive = std::unique_ptr<schema::PrimitiveT>(primitive->UnPack());
  return cnodeT;
}

int TrainExport::LoadModel(void *buf, size_t buf_size) {
  flatbuffers::Verifier verify((const uint8_t *)buf, buf_size);
  if (!schema::VerifyMetaGraphBuffer(verify)) {
    MS_LOG(ERROR) << "model flatbuffer verify fail";
    return RET_ERROR;
  }
  meta_graph_ = schema::GetMetaGraph(buf)->UnPack();
  meta_graph_->outputIndex.clear();
  return RET_OK;
}

std::unique_ptr<schema::TensorT> TrainExport::CreateTransformTensor(size_t id) {
  auto &scTensor = meta_graph_->allTensors.at(id);
  auto tensorT = std::make_unique<schema::TensorT>();
  if (tensorT == nullptr) {
    MS_LOG(ERROR) << "Could not create tensor ";
    return nullptr;
  }
  tensorT->nodeType = scTensor->nodeType;
  tensorT->dataType = scTensor->dataType;
  std::vector<int32_t> dims;
  std::vector<int32_t> val = {0, 2, 3, 1};
  if (scTensor->dims.size() == 4) {
    for (size_t i = 0; i < val.size(); i++) {
      dims.push_back(scTensor->dims.at(val[i]));
    }
    tensorT->dims = dims;
  } else {
    tensorT->dims = scTensor->dims;
  }
  tensorT->format = schema::Format_NHWC;
  tensorT->name = scTensor->name + "_post";
  tensorT->refCount = 0;
  tensorT->offset = 0;
  tensorT->enableHuffmanCode = false;
  return tensorT;
}

std::unique_ptr<schema::TensorT> TrainExport::CreateTransformConst(size_t last_id) {
  auto tensorT = std::make_unique<schema::TensorT>();
  if (tensorT == nullptr) {
    MS_LOG(ERROR) << "Could not create tensor ";
    return nullptr;
  }
  tensorT->nodeType = lite::NodeType_ValueNode;
  tensorT->dataType = TypeId::kNumberTypeInt32;
  tensorT->dims = {4};
  tensorT->format = schema::Format_NCHW;
  tensorT->name = "const-" + std::to_string(last_id);
  tensorT->refCount = 0;
  tensorT->offset = 0;
  tensorT->enableHuffmanCode = false;
  int32_t val[] = {0, 2, 3, 1};
  uint8_t *valp = reinterpret_cast<uint8_t *>(val);
  tensorT->data = std::vector<uint8_t>(valp, valp + sizeof(val));
  return tensorT;
}

std::unique_ptr<schema::CNodeT> TrainExport::CreateTransformNode(std::vector<uint32_t> inputIndex,
                                                                 std::vector<uint32_t> outputIndex, size_t id) {
  auto cnodeT = std::make_unique<schema::CNodeT>();
  if (cnodeT == nullptr) {
    MS_LOG(ERROR) << "cannot allocate node";
    return nullptr;
  }
  cnodeT->inputIndex = inputIndex;
  cnodeT->outputIndex = outputIndex;
  cnodeT->name = "transpose-" + std::to_string(id);
  cnodeT->quantType = schema::QuantType_QUANT_NONE;
  cnodeT->primitive = std::make_unique<schema::PrimitiveT>();
  cnodeT->primitive->value.type = schema::PrimitiveType_Transpose;
  return cnodeT;
}

int TrainExport::AddTransformNode() {
  std::unordered_map<size_t, size_t> reconnect;
  size_t last_id = meta_graph_->allTensors.size();
  size_t last_node = meta_graph_->nodes.size();
  for (auto it : connect_) {
    auto tensorConst = CreateTransformConst(last_id);
    if (tensorConst == nullptr) {
      MS_LOG(ERROR) << "error in create tensor";
      return RET_ERROR;
    }
    meta_graph_->allTensors.emplace_back(std::move(tensorConst));  // last_id
    auto tensorT = CreateTransformTensor(it.second);
    if (tensorT == nullptr) {
      MS_LOG(ERROR) << "error in create tensor";
      return RET_ERROR;
    }
    meta_graph_->allTensors.emplace_back(std::move(tensorT));  // last_id + 1
    std::vector<uint32_t> in_idx = {static_cast<uint32_t>(it.second), static_cast<uint32_t>(last_id)};
    std::vector<uint32_t> out_idx = {static_cast<uint32_t>(last_id + 1)};
    reconnect[it.first] = last_id + 1;
    auto cnode = CreateTransformNode(in_idx, out_idx, last_node);
    if (cnode == nullptr) {
      MS_LOG(ERROR) << "error in node creation";
      return RET_ERROR;
    }
    meta_graph_->nodes.emplace_back(std::move(cnode));
  }
  connect_ = reconnect;
  return RET_OK;
}

int TrainExport::ExportNet(const std::vector<mindspore::kernel::LiteKernel *> &kernels,
                           const std::vector<mindspore::lite::Tensor *> &tensors,
                           const std::vector<std::string> &output_names, const Model *model) {
  std::vector<size_t> map_index;
  std::set<size_t> out_set;
  int offset = meta_graph_->allTensors.size();
  int tensor_idx = offset;

  if (meta_graph_ == nullptr) {
    int status = ExportInit(model->name_, model->version_);
    if (status != RET_OK) {
      return status;
    }
  }
  // prepare mapping for connection
  for (auto it : connect_) {
    remap_[it.first + offset] = it.second;
  }

  for (const auto kernel : kernels) {
    std::vector<uint32_t> in_idx, out_idx;
    for (const auto tensor : kernel->in_tensors()) {
      size_t id = TSFindTensor(tensors, tensor) + offset;
      if (id == tensors.size()) {
        MS_LOG(ERROR) << "cannot find tensor " + tensor->ToString() + " in model";
        return RET_ERROR;
      }
      auto it = remap_.find(id);
      if (it == remap_.end()) {
        remap_[id] = tensor_idx;
        in_idx.push_back(tensor_idx);
        map_index.push_back(id);
        tensor_idx++;
      } else {
        in_idx.push_back(it->second);
      }
    }
    for (const auto tensor : kernel->out_tensors()) {
      size_t id = TSFindTensor(tensors, tensor) + offset;
      if (id == tensors.size()) {
        MS_LOG(ERROR) << "cannot find tensor " + tensor->ToString() + " in model";
        return RET_ERROR;
      }
      auto it = remap_.find(id);
      if (it == remap_.end()) {
        remap_[id] = tensor_idx;
        map_index.push_back(id);
        out_idx.push_back(tensor_idx);
        out_set.insert(tensor_idx);
        tensor_idx++;
      } else {
        out_idx.push_back(it->second);
        out_set.insert(it->second);
      }
    }
    auto cnode = CreateCNode(kernel, in_idx, out_idx, model);
    if (cnode == nullptr) {
      MS_LOG(ERROR) << "failed to create cnode";
      return RET_ERROR;
    }
    meta_graph_->nodes.emplace_back(std::move(cnode));
  }
  for (auto id : map_index) {
    size_t pid = id - offset;
    mindspore::lite::Tensor *tensor = tensors.at(pid);
    schema::Tensor *scTensor = model->all_tensors_.at(pid);
    auto tensorT = CreateTensor(tensor, scTensor);
    if (tensorT == nullptr) {
      MS_LOG(ERROR) << "error in tensor creation";
      return RET_ERROR;
    }
    if (out_set.find(remap_[id]) == out_set.end()) {
      if ((tensorT->nodeType == NodeType_ValueNode) && (tensorT->data.size() == 0)) {
        meta_graph_->inputIndex.push_back(remap_[id]);
      }
    }
    // find output tensor
    if (std::find(output_names.begin(), output_names.end(), tensor->tensor_name()) != output_names.end()) {
      meta_graph_->outputIndex.push_back(remap_[id]);
    }
    meta_graph_->allTensors.emplace_back(std::move(tensorT));
  }
  return RET_OK;
}

int TrainExport::ExportInit(const std::string model_name, std::string version) {
  meta_graph_ = new (std::nothrow) schema::MetaGraphT();
  if (meta_graph_ == nullptr) {
    MS_LOG(ERROR) << "cannot allocate meta_graph";
    return RET_ERROR;
  }
  meta_graph_->fmkType = 3;
  meta_graph_->name = model_name;
  meta_graph_->version = version;
  return RET_OK;
}

int TrainExport::SaveToFile() { return Storage::Save(*meta_graph_, file_name_); }

TrainExport::~TrainExport() { delete meta_graph_; }

}  // namespace lite
}  // namespace mindspore
