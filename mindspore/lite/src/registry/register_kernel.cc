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

#include "include/registry/register_kernel.h"
#include <set>
#include "src/registry/register_kernel_impl.h"

namespace mindspore {
namespace kernel {
int RegisterKernel::RegCustomKernel(const std::string &arch, const std::string &provider, TypeId data_type,
                                    const std::string &type, CreateKernel creator) {
  return lite::RegistryKernelImpl::GetInstance()->RegCustomKernel(arch, provider, data_type, type, creator);
}

int RegisterKernel::RegKernel(const std::string &arch, const std::string &provider, TypeId data_type, int op_type,
                              CreateKernel creator) {
  return lite::RegistryKernelImpl::GetInstance()->RegKernel(arch, provider, data_type, op_type, creator);
}

CreateKernel RegisterKernel::GetCreator(const kernel::KernelDesc &desc, const schema::Primitive *primitive) {
  return lite::RegistryKernelImpl::GetInstance()->GetProviderCreator(desc, primitive);
}
}  // namespace kernel
}  // namespace mindspore
