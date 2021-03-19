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

#ifndef MINDSPORE_CCSRC_BACKEND_OPTIMIZER_CPU_INSERT_CAST_CPU_H
#define MINDSPORE_CCSRC_BACKEND_OPTIMIZER_CPU_INSERT_CAST_CPU_H

#include <string>
#include "backend/optimizer/common/optimizer.h"
#include "ir/anf.h"

namespace mindspore {
namespace opt {
class InsertCastCPU : public PatternProcessPass {
 public:
  explicit InsertCastCPU(bool multigraph = true) : PatternProcessPass("insert_cast_cpu", multigraph) {}
  ~InsertCastCPU() override = default;
  const BaseRef DefinePattern() const override;
  const AnfNodePtr Process(const FuncGraphPtr &, const AnfNodePtr &, const EquivPtr &) const override;
};
}  // namespace opt
}  // namespace mindspore

#endif  // MINDSPORE_CCSRC_BACKEND_OPTIMIZER_CPU_INSERT_CAST_CPU_H
