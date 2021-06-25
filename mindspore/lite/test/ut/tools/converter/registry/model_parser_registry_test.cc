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

#include <vector>
#include "common/common_test.h"
#include "ut/tools/converter/registry/model_parser_test.h"
#include "tools/optimizer/common/gllo_utils.h"
#include "tools/converter/converter_flags.h"

using mindspore::lite::ModelRegistrar;
using mindspore::lite::converter::Flags;
using mindspore::lite::converter::FmkType_CAFFE;
namespace mindspore {
class ModelParserRegistryTest : public mindspore::CommonTest {
 public:
  ModelParserRegistryTest() = default;
};

TEST_F(ModelParserRegistryTest, TestRegistry) {
  auto node_parser_reg = NodeParserTestRegistry::GetInstance();
  auto add_parser = node_parser_reg->GetNodeParser("add");
  ASSERT_NE(add_parser, nullptr);
  auto proposal_parser = node_parser_reg->GetNodeParser("proposal");
  ASSERT_NE(proposal_parser, nullptr);
  REG_MODEL_PARSER(FmkType_CAFFE,
                   TestModelParserCreator);  // register test model parser creator, which will overwrite existing.
  auto model_parser = lite::ModelParserRegistry::GetInstance()->GetModelParser(FmkType_CAFFE);
  ASSERT_NE(model_parser, nullptr);
  Flags flags;
  auto func_graph = model_parser->Parse(flags);
  ASSERT_NE(func_graph, nullptr);
  auto node_list = func_graph->GetOrderedCnodes();
  ASSERT_EQ(node_list.size(), 3);
  auto iter = node_list.begin();
  bool is_add = opt::CheckPrimitiveType(*iter, prim::kPrimAddFusion);
  ASSERT_EQ(is_add, true);
  ++iter;
  is_add = opt::CheckPrimitiveType(*iter, prim::kPrimAddFusion);
  ASSERT_EQ(is_add, true);
  ++iter;
  bool is_return = opt::CheckPrimitiveType(*iter, prim::kPrimReturn);
  ASSERT_EQ(is_return, true);
}
}  // namespace mindspore
