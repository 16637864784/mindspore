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
#include "include/registry/kernel_interface.h"
#include <set>
#include <utility>
#include "src/registry/kernel_interface_registry.h"

namespace mindspore {
namespace kernel {
int RegisterKernelInterface::Reg(const std::string &provider, int op_type, KernelInterfaceCreator creator) {
  return lite::KernelInterfaceRegistry::Instance()->Reg(provider, op_type, creator);
}

int RegisterKernelInterface::CustomReg(const std::string &provider, const std::string &op_type,
                                       KernelInterfaceCreator creator) {
  return lite::KernelInterfaceRegistry::Instance()->CustomReg(provider, op_type, creator);
}

bool RegisterKernelInterface::CheckReg(const lite::Model::Node *node, std::set<std::string> &&providers) {
  return lite::KernelInterfaceRegistry::Instance()->CheckReg(node, std::forward<std::set<std::string>>(providers));
}

std::shared_ptr<kernel::KernelInterface> RegisterKernelInterface::GetKernelInterface(
  const std::string &provider, const schema::Primitive *primitive) {
  return lite::KernelInterfaceRegistry::Instance()->GetKernelInterface(provider, primitive);
}
}  // namespace kernel
}  // namespace mindspore
