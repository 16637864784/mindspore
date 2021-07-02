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

#ifndef MINDSPORE_LITE_INCLUDE_REGISTRY_REGISTER_KERNEL_H_
#define MINDSPORE_LITE_INCLUDE_REGISTRY_REGISTER_KERNEL_H_

#include <set>
#include <string>
#include <vector>
#include <memory>
#include "schema/model_generated.h"
#include "include/context.h"
#include "include/ms_tensor.h"
#include "include/kernel.h"

namespace mindspore {
namespace kernel {
/// \brief KernelDesc defined kernel's basic attribute.
struct MS_API KernelDesc {
  TypeId data_type;     /**< kernel data type argument */
  int type;             /**< op type argument */
  std::string arch;     /**< deviceType argument */
  std::string provider; /**< user identification argument */

  bool operator<(const KernelDesc &dst) const {
    if (provider != dst.provider) {
      return provider < dst.provider;
    } else if (arch != dst.arch) {
      return arch < dst.arch;
    } else if (data_type != dst.data_type) {
      return data_type < dst.data_type;
    } else {
      return type < dst.type;
    }
  }
};

/// \brief CreateKernel Defined a functor to create a kernel.
///
/// \param[in] inputs Define input tensors of kernel.
/// \param[in] outputs Define output tensors of kernel.
/// \param[in] primitive Define attributes of op.
/// \param[in] ctx Define for holding environment variables during runtime.
///
/// \return Smart Pointer of kernel.
using CreateKernel = std::function<std::shared_ptr<kernel::Kernel>(
  const std::vector<tensor::MSTensor *> &inputs, const std::vector<tensor::MSTensor *> &outputs,
  const schema::Primitive *primitive, const lite::Context *ctx)>;

/// \brief RegisterKernel Defined registration of kernel.
class MS_API RegisterKernel {
 public:
  /// \brief Static method to register kernel which is correspondng to an ordinary op.
  ///
  /// \param[in] arch Define deviceType, such as CPU.
  /// \param[in] provider Define the identification of user.
  /// \param[in] data_type Define kernel's input data type.
  /// \param[in] type Define the ordinary op type.
  /// \param[in] creator Define a function pointer to create a kernel.
  ///
  /// \return STATUS as an error code of registering, STATUS is defined in errorcode.h.
  static int RegKernel(const std::string &arch, const std::string &provider, TypeId data_type, int type,
                       CreateKernel creator);

  /// \brief Static method to register kernel which is corresponding to custom op.
  ///
  /// \param[in] arch Define deviceType, such as CPU.
  /// \param[in] provider Define the identification of user.
  /// \param[in] data_type Define kernel's input data type.
  /// \param[in] type Define the concrete type of a custom op.
  /// \param[in] creator Define a function pointer to create a kernel.
  ///
  /// \return STATUS as an error code of registering, STATUS is defined in errorcode.h.
  static int RegCustomKernel(const std::string &arch, const std::string &provider, TypeId data_type,
                             const std::string &type, CreateKernel creator);

  /// \brief Static methon to get a kernel's create function.
  ///
  /// \param[in] desc Define kernel's basic attribute.
  /// \param[in] primitive Define the attributes of op.
  ///
  /// \return Function pointer to create a kernel.
  static CreateKernel GetCreator(const schema::Primitive *primitive, kernel::KernelDesc *desc);
};

/// \brief KernelReg Defined registration class of kernel.
class MS_API KernelReg {
 public:
  /// \brief Destructor of KernelReg.
  ~KernelReg() = default;

  /// \brief Method to register ordinary op.
  ///
  /// \param[in] arch Define deviceType, such as CPU.
  /// \param[in] provider Define the identification of user.
  /// \param[in] data_type Define kernel's input data type.
  /// \param[in] op_type Define the ordinary op type.
  /// \param[in] creator Define a function pointer to create a kernel.
  KernelReg(const std::string &arch, const std::string &provider, TypeId data_type, int op_type, CreateKernel creator) {
    RegisterKernel::RegKernel(arch, provider, data_type, op_type, creator);
  }

  /// \brief Method to register customized op.
  ///
  /// \param[in] arch Define deviceType, such as CPU.
  /// \param[in] provider Define the identification of user.
  /// \param[in] data_type Define kernel's input data type.
  /// \param[in] op_type Define the concrete type of a custom op.
  /// \param[in] creator Define a function pointer to create a kernel.
  KernelReg(const std::string &arch, const std::string &provider, TypeId data_type, const std::string &op_type,
            CreateKernel creator) {
    RegisterKernel::RegCustomKernel(arch, provider, data_type, op_type, creator);
  }
};

/// \brief Defined registering macro to register ordinary op kernel, which called by user directly.
///
/// \param[in] arch Define deviceType, such as CPU.
/// \param[in] provider Define the identification of user.
/// \param[in] data_type Define kernel's input data type.
/// \param[in] op_type Define the ordinary op type.
/// \param[in] creator Define a function pointer to create a kernel.
#define REGISTER_KERNEL(arch, provider, data_type, op_type, creator)                                                 \
  namespace {                                                                                                        \
  static mindspore::kernel::KernelReg g_##arch##provider##data_type##op_type##kernelReg(#arch, #provider, data_type, \
                                                                                        op_type, creator);           \
  }  // namespace

/// \brief Defined registering macro to register custom op kernel, which called by user directly.
///
/// \param[in] arch Define deviceType, such as CPU.
/// \param[in] provider Define the identification of user.
/// \param[in] data_type Define kernel's input data type.
/// \param[in] op_type Define the concrete type of a custom op.
/// \param[in] creator Define a function pointer to create a kernel.
#define REGISTER_CUSTOM_KERNEL(arch, provider, data_type, op_type, creator)                                          \
  namespace {                                                                                                        \
  static mindspore::kernel::KernelReg g_##arch##provider##data_type##op_type##kernelReg(#arch, #provider, data_type, \
                                                                                        #op_type, creator);          \
  }  // namespace
}  // namespace kernel
}  // namespace mindspore

#endif  // MINDSPORE_LITE_INCLUDE_REGISTRY_REGISTER_KERNEL_H_
