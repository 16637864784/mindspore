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

#include "tools/converter/parser/onnx/onnx_slice_parser.h"
#include <memory>
#include <vector>

namespace mindspore {
namespace lite {
STATUS OnnxSliceParser::Parse(const onnx::GraphProto &onnx_graph, const onnx::NodeProto &onnx_node,
                              schema::CNodeT *op) {
  MS_LOG(DEBUG) << "onnx SliceParser";
  if (op == nullptr) {
    MS_LOG(ERROR) << "op is null";
    return RET_NULL_PTR;
  }
  op->primitive = std::make_unique<schema::PrimitiveT>();
  if (op->primitive == nullptr) {
    MS_LOG(ERROR) << "op->primitive is null";
    return RET_NULL_PTR;
  }

  std::unique_ptr<schema::SliceT> attr = std::make_unique<schema::SliceT>();
  if (attr == nullptr) {
    MS_LOG(ERROR) << "new op failed";
    return RET_NULL_PTR;
  }

  std::vector<int> axes;
  std::vector<int> starts;
  std::vector<int> ends;
  std::vector<int> steps;
  for (const auto &onnx_node_attr : onnx_node.attribute()) {
    const auto &attribute_name = onnx_node_attr.name();
    if (attribute_name == "starts") {
      const int num = onnx_node_attr.ints_size();
      starts.clear();
      for (int i = 0; i < num; ++i) {
        starts.push_back(static_cast<int>(onnx_node_attr.ints()[i]));
      }
    } else if (attribute_name == "axes") {
      const int num = onnx_node_attr.ints_size();
      axes.clear();
      for (int i = 0; i < num; ++i) {
        axes.push_back(static_cast<int>(onnx_node_attr.ints()[i]));
      }
    } else if (attribute_name == "ends") {
      const int num = onnx_node_attr.ints_size();
      ends.clear();
      for (int i = 0; i < num; ++i) {
        ends.push_back(static_cast<int>(onnx_node_attr.ints()[i]));
      }
    } else if (attribute_name == "steps") {
      const int num = onnx_node_attr.ints_size();
      steps.clear();
      for (int i = 0; i < num; ++i) {
        steps.push_back(static_cast<int>(onnx_node_attr.ints()[i]));
      }
    }
  }

  if (onnx_node.input_size() > 1) {
    const auto &starts_name = onnx_node.input(1);
    for (const auto &it : onnx_graph.initializer()) {
      if (it.name() == starts_name) {
        starts.clear();
        for (int i = 0; i < it.int32_data_size(); ++i) {
          starts.push_back(it.int32_data(i));
        }
      }
    }
  }

  if (onnx_node.input_size() > 2) {
    const auto &ends_name = onnx_node.input(2);
    for (const auto &it : onnx_graph.initializer()) {
      if (it.name() == ends_name) {
        ends.clear();
        for (int i = 0; i < it.int32_data_size(); ++i) {
          ends.push_back(it.int32_data(i));
        }
      }
    }
  }

  if (onnx_node.input_size() > 3) {
    const auto &axes_name = onnx_node.input(3);
    for (const auto &it : onnx_graph.initializer()) {
      if (it.name() == axes_name) {
        axes.clear();
        for (int i = 0; i < it.int32_data_size(); ++i) {
          axes.push_back(it.int32_data(i));
        }
      }
    }
  }

  if (onnx_node.input_size() > 4) {
    const auto &steps_name = onnx_node.input(4);
    for (const auto &it : onnx_graph.initializer()) {
      if (it.name() == steps_name) {
        steps.clear();
        for (int i = 0; i < it.int32_data_size(); ++i) {
          steps.push_back(it.int32_data(i));
        }
      }
    }
  }

  std::vector<int> sizes(starts.size(), -1);
  for (size_t i = 0; i < starts.size(); ++i) {
    sizes[i] = (ends[i] < 0 ? ends[i] : ends[i] - starts[i]);
  }
  attr->axes = axes;
  attr->begin = starts;
  attr->size = sizes;
  attr->step = steps;
  op->primitive->value.type = schema::PrimitiveType_Slice;
  op->primitive->value.value = attr.release();
  return RET_OK;
}

OnnxNodeRegistrar g_onnxSliceParser("Slice", new OnnxSliceParser());
}  // namespace lite
}  // namespace mindspore
