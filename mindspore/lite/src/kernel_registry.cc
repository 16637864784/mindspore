/**
 * Copyright 2020-2021 Huawei Technologies Co., Ltd
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
#include "src/kernel_registry.h"
#include <utility>
#include "include/errorcode.h"
#include "src/registry/register_kernel.h"
#include "src/ops/populate/populate_register.h"
#include "src/common/version_manager.h"
#include "nnacl/pooling_parameter.h"
#if defined(ENABLE_FP16) && defined(ENABLE_ARM)
#if defined(__ANDROID__)
#include <asm/hwcap.h>
#endif
#include "common/utils.h"
#include "src/common/log_adapter.h"
#include "src/common/utils.h"
#endif
#include "src/common/tensor_util.h"

using mindspore::kernel::CreateKernel;
using mindspore::kernel::kBuiltin;
using mindspore::kernel::kCPU;
using mindspore::kernel::KERNEL_ARCH;
using mindspore::kernel::KernelCreator;
using mindspore::kernel::KernelKey;

namespace mindspore::lite {
namespace {
void KernelKeyToKernelDesc(const KernelKey &key, kernel::KernelDesc *desc) {
  MS_ASSERT(desc != nullptr);
  desc->data_type = key.data_type;
  desc->type = key.type;
  desc->arch = key.kernel_arch;
  desc->provider = key.provider;
}
}  // namespace

KernelRegistry *KernelRegistry::GetInstance() {
  static KernelRegistry instance;

  std::unique_lock<std::mutex> malloc_creator_array(instance.lock_);
  if (instance.creator_arrays_ == nullptr) {
    instance.creator_arrays_ = reinterpret_cast<KernelCreator *>(malloc(array_size_ * sizeof(KernelCreator)));
    if (instance.creator_arrays_ == nullptr) {
      return nullptr;
    }
    memset(instance.creator_arrays_, 0, array_size_ * sizeof(KernelCreator));
  }
  return &instance;
}

int KernelRegistry::Init() { return RET_OK; }

kernel::KernelCreator KernelRegistry::GetCreator(const KernelKey &desc) {
  if (desc.provider == kBuiltin) {
    int index = GetCreatorFuncIndex(desc);
    if (index >= array_size_ || index < 0) {
      MS_LOG(ERROR) << "invalid kernel key, arch " << desc.arch << ", data_type " << desc.data_type << ",op type "
                    << desc.type;
      return nullptr;
    }
    return creator_arrays_[index];
  }
  MS_LOG(ERROR) << "Call wrong interface!provider: " << desc.provider;
  return nullptr;
}

int KernelRegistry::GetCreatorFuncIndex(const kernel::KernelKey desc) {
  int index;
  int device_index = static_cast<int>(desc.arch) - kKernelArch_MIN;
  int dType_index = static_cast<int>(desc.data_type) - kNumberTypeBegin;
  int op_index = static_cast<int>(desc.type);
  index = device_index * data_type_length_ * op_type_length_ + dType_index * op_type_length_ + op_index;
  return index;
}

void KernelRegistry::RegKernel(const KernelKey desc, const kernel::KernelCreator creator) {
  int index = GetCreatorFuncIndex(desc);
  if (index >= array_size_) {
    MS_LOG(ERROR) << "invalid kernel key, arch " << desc.arch << ", data_type" << desc.data_type << ",op type "
                  << desc.type;
    return;
  }
  creator_arrays_[index] = creator;
}

void KernelRegistry::RegKernel(KERNEL_ARCH arch, TypeId data_type, int op_type, kernel::KernelCreator creator) {
  KernelKey desc = {arch, data_type, op_type};
  int index = GetCreatorFuncIndex(desc);
  if (index >= array_size_) {
    MS_LOG(ERROR) << "invalid kernel key, arch " << desc.arch << ", data_type" << desc.data_type << ",op type "
                  << desc.type;
    return;
  }
  creator_arrays_[index] = creator;
}

bool KernelRegistry::Merge(const std::unordered_map<KernelKey, KernelCreator> &new_creators) { return false; }

KernelRegistry::~KernelRegistry() {
  KernelRegistry *instance = GetInstance();
  std::unique_lock<std::mutex> malloc_creator_array(instance->lock_);
  if (instance->creator_arrays_ != nullptr) {
    free(instance->creator_arrays_);
    instance->creator_arrays_ = nullptr;
  }
}

bool KernelRegistry::SupportKernel(const KernelKey &key) {
  auto kernel_creator = GetCreator(key);
  return kernel_creator != nullptr;
}

int KernelRegistry::GetKernel(const std::vector<Tensor *> &in_tensors, const std::vector<Tensor *> &out_tensors,
                              const InnerContext *ctx, const kernel::KernelKey &key, OpParameter *parameter,
                              kernel::LiteKernel **kernel, const void *primitive) {
  MS_ASSERT(ctx != nullptr);
  MS_ASSERT(kernel != nullptr);
  if (key.provider == kBuiltin) {
    auto creator = GetCreator(key);
    if (creator != nullptr) {
      auto inner_kernel = creator(in_tensors, out_tensors, parameter, ctx, key);
      if (inner_kernel != nullptr) {
        inner_kernel->set_registry_data_type(key.data_type);
        auto *lite_kernel = new (std::nothrow) kernel::LiteKernel(inner_kernel);
        if (lite_kernel != nullptr) {
          lite_kernel->set_desc(key);
          *kernel = lite_kernel;
          return RET_OK;
        } else {
          delete inner_kernel;
        }
      }
      return RET_ERROR;
    }
  } else {
    kernel::KernelDesc desc;
    KernelKeyToKernelDesc(key, &desc);
    auto creator =
      kernel::RegisterKernel::GetInstance()->GetCreator(desc, static_cast<const schema::Primitive *>(primitive));
    if (creator == nullptr) {
      return RET_NOT_SUPPORT;
    }
    std::vector<tensor::MSTensor *> tensors_in(in_tensors.begin(), in_tensors.end());
    std::vector<tensor::MSTensor *> tensors_out(out_tensors.begin(), out_tensors.end());
    auto base_kernel = creator(tensors_in, tensors_out, static_cast<const schema::Primitive *>(primitive), ctx);
    if (base_kernel != nullptr) {
      auto *lite_kernel = new (std::nothrow) kernel::LiteKernel(base_kernel.get());
      if (lite_kernel != nullptr) {
        lite_kernel->set_desc(key);
        *kernel = lite_kernel;
        return RET_OK;
      }
    }
    return RET_ERROR;
  }
  return RET_NOT_SUPPORT;
}
}  // namespace mindspore::lite
