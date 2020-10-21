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

#include "src/runtime/kernel/arm/int8/scale_int8.h"

#include <string.h>
#include <vector>
#include "nnacl/int8/scale_int8.h"
#include "schema/model_generated.h"
#include "src/kernel_registry.h"
#include "include/errorcode.h"
#include "src/runtime/runtime_api.h"

using mindspore::lite::KernelRegistrar;
using mindspore::lite::RET_ERROR;
using mindspore::lite::RET_OK;
using mindspore::schema::PrimitiveType_Scale;

namespace mindspore::kernel {
namespace {
constexpr size_t kScaleInputsSize = 2;
constexpr size_t kScaleBiasInputsSize = 3;
}  // namespace
ScaleInt8CPUKernel::~ScaleInt8CPUKernel() {
  if (scale_param_->const_scale_) {
    if (scale_ != nullptr) {
      free(scale_);
      scale_ = nullptr;
    }
  }
  if (has_bias_ && scale_param_->const_offset_) {
    if (offset_ != nullptr) {
      free(offset_);
      offset_ = nullptr;
    }
  }
}

int ScaleInt8CPUKernel::InitScaleOffset() {
  auto scale_tensor = in_tensors_.at(1);
  int8_t *scale_ptr = reinterpret_cast<int8_t *>(in_tensors_.at(1)->data_c());
  if (scale_ptr != nullptr) {
    scale_param_->const_scale_ = true;
    if (scale_ != nullptr) {
      free(scale_);
      scale_ = nullptr;
    }
    scale_ = reinterpret_cast<int8_t *>(malloc(scale_tensor->ElementsNum() * sizeof(int8_t)));
    if (scale_ == nullptr) {
      MS_LOG(ERROR) << "Malloc buffer failed.";
      return RET_ERROR;
    }
    memcpy(scale_, scale_ptr, scale_tensor->ElementsNum() * sizeof(int8_t));
  } else {
    scale_param_->const_scale_ = false;
    scale_ = nullptr;
  }

  if (in_tensors_.size() == 3) {
    has_bias_ = true;
    auto offset_tensor = in_tensors_.at(2);
    int8_t *offset_ptr = reinterpret_cast<int8_t *>(offset_tensor->data_c());
    if (offset_ptr != nullptr) {
      scale_param_->const_offset_ = true;
      if (offset_ != nullptr) {
        free(offset_);
        offset_ = nullptr;
      }
      offset_ = reinterpret_cast<int8_t *>(malloc(offset_tensor->ElementsNum() * sizeof(int8_t)));
      if (offset_ == nullptr) {
        MS_LOG(ERROR) << "Malloc buffer failed.";
        return RET_ERROR;
      }
      memcpy(offset_, offset_ptr, offset_tensor->ElementsNum() * sizeof(int8_t));
    } else {
      scale_param_->const_offset_ = false;
      offset_ = nullptr;
    }
  } else {
    has_bias_ = false;
  }
  return RET_OK;
}

int ScaleInt8CPUKernel::InitParameter() {
  auto in_tensor = in_tensors_.at(0);
  auto in_shape = in_tensor->shape();
  auto scale_tensor = in_tensors_.at(1);
  auto scale_shape = scale_tensor->shape();

  if (scale_param_->axis_ < 0) {
    scale_param_->axis_ = scale_param_->axis_ + in_shape.size();
  }
  if (scale_shape.size() + scale_param_->axis_ > in_shape.size()) {
    MS_LOG(ERROR) << "Scale tensor shape is incorrect.";
    return RET_ERROR;
  }
  scale_param_->outer_size_ = 1;
  scale_param_->axis_size_ = 1;
  scale_param_->inner_size_ = 1;
  for (int i = 0; i < scale_param_->axis_; i++) {
    scale_param_->outer_size_ *= in_shape[i];
  }
  for (size_t i = 0; i < scale_shape.size(); i++) {
    if (in_shape[i + scale_param_->axis_] != scale_shape[i]) {
      MS_LOG(ERROR) << "Scale tensor shape is incorrect.";
      return RET_ERROR;
    }
    scale_param_->axis_size_ *= in_shape[i + scale_param_->axis_];
  }
  for (size_t i = scale_param_->axis_ + scale_shape.size(); i < in_shape.size(); i++) {
    scale_param_->inner_size_ *= in_shape[i];
  }
  scale_param_->op_parameter_.thread_num_ = MSMIN(scale_param_->op_parameter_.thread_num_, scale_param_->outer_size_);
  return RET_OK;
}

int ScaleInt8CPUKernel::InitQuantArgs() {
  auto input = in_tensors_.at(0);
  auto scale = in_tensors_.at(1);
  auto output = out_tensors_.at(0);
  auto input_scale = input->GetQuantParams().front().scale;
  auto scale_scale = scale->GetQuantParams().front().scale;
  auto output_scale = output->GetQuantParams().front().scale;
  scale_param_->input_zp_ = input->GetQuantParams().front().zeroPoint;
  scale_param_->scale_zp_ = scale->GetQuantParams().front().zeroPoint;
  scale_param_->output_zp_ = output->GetQuantParams().front().zeroPoint;

  // (in * scale + offset) / output
  const double input_output_multiplier = input_scale * scale_scale / output_scale;
  int shift;
  QuantizeMultiplier(input_output_multiplier, &scale_param_->scale_mul_arg_.multiplier_, &shift);
  scale_param_->scale_mul_arg_.left_shift_ = shift > 0 ? shift : 0;
  scale_param_->scale_mul_arg_.right_shift_ = shift < 0 ? -shift : 0;

  if (in_tensors_.size() == kScaleBiasInputsSize) {
    auto offset = in_tensors_.at(2);
    auto offset_scale = offset->GetQuantParams().front().scale;
    scale_param_->offset_zp_ = offset->GetQuantParams().front().zeroPoint;

    const double offset_multiplier = offset_scale / output_scale;
    QuantizeMultiplier(offset_multiplier, &scale_param_->offset_mul_arg_.multiplier_, &shift);
    scale_param_->offset_mul_arg_.left_shift_ = shift > 0 ? shift : 0;
    scale_param_->offset_mul_arg_.right_shift_ = shift < 0 ? -shift : 0;
  }
  return RET_OK;
}

int ScaleInt8CPUKernel::Init() {
  if (in_tensors_.size() < kScaleInputsSize || in_tensors_.size() > kScaleBiasInputsSize) {
    MS_LOG(ERROR) << "inputs to Scale operator should be 2 or 3, but " << in_tensors_.size() << " is given.";
    return RET_ERROR;
  }

  if (!InferShapeDone()) {
    return RET_OK;
  }

  ReSize();
  return RET_OK;
}

int ScaleInt8CPUKernel::ReSize() {
  auto ret = InitParameter();
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "Scale fp32 InitParameter failed.";
    return RET_ERROR;
  }

  ret = InitScaleOffset();
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "Scale fp32 InitScaleOffset failed.";
    return RET_ERROR;
  }

  ret = InitQuantArgs();
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "Scale fp32 InitQuantArgs failed.";
    return ret;
  }
  return RET_OK;
}

int ScaleInt8CPUKernel::Scale(int task_id) {
  if (has_bias_) {
    switch (scale_param_->activation_type_) {
      case schema::ActivationType_RELU:
        DoScaleWithBiasInt8(input_ptr_, output_ptr_, scale_, offset_, task_id, scale_param_, INT8_MAX, 0);
        break;
      case schema::ActivationType_RELU6:
        DoScaleWithBiasInt8(input_ptr_, output_ptr_, scale_, offset_, task_id, scale_param_, 6, 0);
        break;
      case schema::ActivationType_NO_ACTIVATION:
        DoScaleWithBiasInt8(input_ptr_, output_ptr_, scale_, offset_, task_id, scale_param_, INT8_MAX, INT8_MIN);
        break;
      default:
        MS_LOG(ERROR) << "Scale does not support activation type " << scale_param_->activation_type_;
        return RET_ERROR;
    }
  } else {
    switch (scale_param_->activation_type_) {
      case schema::ActivationType_RELU:
        DoScaleInt8(input_ptr_, output_ptr_, scale_, task_id, scale_param_, INT8_MAX, 0);
        break;
      case schema::ActivationType_RELU6:
        DoScaleInt8(input_ptr_, output_ptr_, scale_, task_id, scale_param_, 6, 0);
        break;
      case schema::ActivationType_NO_ACTIVATION:
        DoScaleInt8(input_ptr_, output_ptr_, scale_, task_id, scale_param_, INT8_MAX, INT8_MIN);
        break;
      default:
        MS_LOG(ERROR) << "Scale does not support activation type " << scale_param_->activation_type_;
        return RET_ERROR;
    }
  }

  return RET_OK;
}

int ScaleRunInt8(void *cdata, int task_id) {
  auto scale = reinterpret_cast<ScaleInt8CPUKernel *>(cdata);
  auto ret = scale->Scale(task_id);
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "ScaleRunInt8 error task_id[" << task_id << "] error_code[" << ret << "]";
    return RET_ERROR;
  }
  return RET_OK;
}

int ScaleInt8CPUKernel::Run() {
  auto in_tensor = in_tensors_.front();
  input_ptr_ = reinterpret_cast<int8_t *>(in_tensor->data_c());
  if (scale_ == nullptr) {
    auto scale_tensor = in_tensors_[1];
    scale_ = reinterpret_cast<int8_t *>(scale_tensor->data_c());
  }
  if (has_bias_ && !scale_param_->const_offset_) {
    offset_ = reinterpret_cast<int8_t *>(in_tensors_.at(2)->data_c());
  }
  auto out_tensor = out_tensors_.front();
  output_ptr_ = reinterpret_cast<int8_t *>(out_tensor->data_c());

  auto ret = ParallelLaunch(this->context_->thread_pool_, ScaleRunInt8, this, op_parameter_->thread_num_);
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "Scale error error_code[" << ret << "]";
    return RET_ERROR;
  }
  return RET_OK;
}
kernel::LiteKernel *CpuScaleInt8KernelCreator(const std::vector<lite::Tensor *> &inputs,
                                              const std::vector<lite::Tensor *> &outputs, OpParameter *opParameter,
                                              const lite::InnerContext *ctx, const kernel::KernelKey &desc,
                                              const mindspore::lite::PrimitiveC *primitive) {
  MS_ASSERT(desc.type == schema::PrimitiveType_Scale);
  if (opParameter == nullptr) {
    MS_LOG(ERROR) << "opParameter is nullptr";
    return nullptr;
  }
  auto *kernel = new (std::nothrow) ScaleInt8CPUKernel(opParameter, inputs, outputs, ctx, primitive);
  if (kernel == nullptr) {
    MS_LOG(ERROR) << "New kernel fails.";
    free(opParameter);
    return nullptr;
  }

  auto ret = kernel->Init();
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "Init kernel failed, name: " << opParameter->name_ << ", type: "
                  << schema::EnumNamePrimitiveType(static_cast<schema::PrimitiveType>(opParameter->type_));
    delete kernel;
    return nullptr;
  }
  return kernel;
}

REG_KERNEL(kCPU, kNumberTypeInt8, PrimitiveType_Scale, CpuScaleInt8KernelCreator)
}  // namespace mindspore::kernel
