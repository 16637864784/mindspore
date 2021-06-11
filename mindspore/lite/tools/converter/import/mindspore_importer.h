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

#ifndef MINDSPORE_LITE_TOOLS_IMPORT_MINDSPORE_IMPORTER_H_
#define MINDSPORE_LITE_TOOLS_IMPORT_MINDSPORE_IMPORTER_H_

#include <string>
#include "tools/converter/converter_flags.h"
#include "load_mindir/load_model.h"

namespace mindspore::lite {
class MindsporeImporter {
 public:
  MindsporeImporter() = default;
  ~MindsporeImporter() = default;
  FuncGraphPtr ImportMindIR(const converter::Flags &flag);

 private:
  STATUS Mindir2AnfAdjust(const FuncGraphPtr &func_graph, const converter::Flags &flag);
  STATUS WeightFormatTransform(const FuncGraphPtr &graph);
  STATUS HardCodeMindir(const CNodePtr &conv_node, const FuncGraphPtr &graph);
  QuantType quant_type_ = schema::QuantType_QUANT_NONE;
  size_t Hex2ByteArray(std::string hex_str, unsigned char *byte_array, size_t max_len);
};

}  // namespace mindspore::lite
#endif  // MINDSPORE_LITE_TOOLS_IMPORT_MINDSPORE_IMPORTER_H_
