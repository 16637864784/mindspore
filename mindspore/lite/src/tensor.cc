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

#include "src/tensor.h"
#include <vector>
#include <string>
#include <utility>
#include <algorithm>
#include <functional>
#include "securec/include/securec.h"
#include "include/errorcode.h"

namespace mindspore {
namespace lite {
namespace {
constexpr int kMaxMallocSize = 1024 * 1024 * 300;
}  // namespace
Tensor::Tensor(const TypeId data_type, std::vector<int> shape, const mindspore::Format &format, Category category)
    : data_type_(data_type), shape_(std::move(shape)), format_(format), category_(category) {}

int Tensor::CopyTensorData(const Tensor &src_tensor, Tensor *dst_tensor) {
  if (dst_tensor == nullptr) {
    MS_LOG(ERROR) << "dst_tensor is nullptr";
    return RET_PARAM_INVALID;
  }
  if (src_tensor.data_ == nullptr) {
    MS_LOG(INFO) << "data of src tensor is nullptr";
    return RET_OK;
  }
  size_t data_size = dst_tensor->Size();
  if (data_size != src_tensor.Size()) {
    MS_LOG(ERROR) << "Size of dst tensor is not compatible with src tensor";
    return RET_ERROR;
  }
  if (dst_tensor->MallocData() != RET_OK) {
    MS_LOG(ERROR) << "Malloc memory failed";
    return RET_ERROR;
  }
  dst_tensor->ResetRefCount();
  memcpy(dst_tensor->data_, src_tensor.data_, data_size);
  return RET_OK;
}

Tensor *Tensor::CopyTensor(const Tensor &src_tensor, bool copy_data, AllocatorPtr allocator) {
  auto *result = new (std::nothrow) Tensor;
  if (result == nullptr) {
    MS_LOG(ERROR) << "New tensor failed";
    return nullptr;
  }
  result->data_type_ = src_tensor.data_type_;
  result->shape_ = src_tensor.shape_;
  result->category_ = src_tensor.category_;
  result->format_ = src_tensor.format_;
  result->set_allocator(allocator);
  if (copy_data) {
    auto ret = CopyTensorData(src_tensor, result);
    if (ret != RET_OK) {
      MS_LOG(ERROR) << "CopyTensorData error";
      return nullptr;
    }
    result->own_data_ = src_tensor.own_data_;
  }
  return result;
}

Tensor::~Tensor() {
  if (this->data_ != nullptr && this->own_data_) {
    if (this->allocator_ != nullptr) {
      this->allocator_->Free(this->data_);
    } else {
      free(this->data_);
    }
    this->data_ = nullptr;
  }
}

bool Tensor::operator==(const Tensor &tensor) {
  return data_ == tensor.data_ && shape_ == tensor.shape_ && data_type_ == tensor.data_type_;
}

int32_t Tensor::Batch() const {
  if (this->shape_.size() != 4 && this->shape_.size() != 2) {
    MS_LOG(ERROR) << "Unsupported tensor shape: " << this->shape().size();
    return RET_ERROR;
  }
  switch (this->format_) {
    case mindspore::NHWC:
    case mindspore::NHWC4:
    case mindspore::NCHW:
    case mindspore::NC4HW4:
    case mindspore::KCHW:
    case mindspore::KHWC:
    case mindspore::NC:
    case mindspore::NC4:
      return this->shape_[0];
    case mindspore::HWCK:
    case mindspore::CHWK:
      return this->shape_[3];
    case mindspore::HWKC:
      return this->shape_[2];
    case mindspore::CKHW:
      return this->shape_[1];
    default:
      MS_LOG(ERROR) << "Unsupported format: " << EnumNameFormat(static_cast<schema::Format>(this->format_));
      return RET_ERROR;
  }
}

int32_t Tensor::Channel() const {
  if (this->shape_.size() != 4 && this->shape_.size() != 2) {
    MS_LOG(ERROR) << "Unsupported tensor shape: " << this->shape().size();
    return RET_ERROR;
  }
  switch (this->format_) {
    case mindspore::NCHW:
    case mindspore::KCHW:
    case mindspore::NC:
    case mindspore::NC4:
      return this->shape_[1];
    case mindspore::HWCK:
      return this->shape_[2];
    case mindspore::HWKC:
    case mindspore::NHWC:
    case mindspore::NHWC4:
    case mindspore::NC4HW4:
    case mindspore::KHWC:
      return this->shape_[3];
    case mindspore::CKHW:
    case mindspore::CHWK:
      return this->shape_[0];
    default:
      return RET_ERROR;
  }
}

int32_t Tensor::Height() const {
  if (this->shape_.size() != 4 && this->shape_.size() != 2) {
    MS_LOG(ERROR) << "Unsupported tensor shape: " << this->shape().size();
    return RET_ERROR;
  }
  switch (this->format_) {
    case mindspore::NCHW:
    case mindspore::KCHW:
    case mindspore::CKHW:
      return this->shape_[2];
    case mindspore::NHWC:
    case mindspore::NHWC4:
    case mindspore::NC4HW4:
    case mindspore::KHWC:
    case mindspore::CHWK:
      return this->shape_[1];
    case mindspore::HWCK:
    case mindspore::HWKC:
    case mindspore::HW:
    case mindspore::HW4:
      return this->shape_[0];
    default:
      MS_LOG(ERROR) << "Unsupported format: " << EnumNameFormat(static_cast<schema::Format>(this->format_));
      return RET_ERROR;
  }
}

int32_t Tensor::Width() const {
  if (this->shape_.size() != 4 && this->shape_.size() != 2) {
    MS_LOG(ERROR) << "Unsupported tensor shape: " << this->shape().size();
    return -1;
  }
  switch (this->format_) {
    case mindspore::NCHW:
    case mindspore::KCHW:
    case mindspore::CKHW:
      return this->shape_[3];
    case mindspore::KHWC:
    case mindspore::NHWC:
    case mindspore::NHWC4:
    case mindspore::NC4HW4:
    case mindspore::CHWK:
      return this->shape_[2];
    case mindspore::HWCK:
    case mindspore::HWKC:
    case mindspore::HW:
    case mindspore::HW4:
      return this->shape_[1];
    default:
      return RET_ERROR;
  }
}

size_t Tensor::Size() const {
  size_t element_size = DataTypeSize(this->data_type_);
  auto element_num = (format_ == mindspore::NC4HW4 || format_ == mindspore::NHWC4) ? ElementsC4Num() : ElementsNum();
  if (element_num < 0) {
    MS_LOG(INFO) << "Element number of tensor should large than 0 : " << element_num;
    return 0;
  }
  return element_size * element_num;
}

int Tensor::ElementsNum() const {
  if (this->category_ == CONST_SCALAR) {
    return 1;
  }
  auto num = std::accumulate(shape_.begin(), shape_.end(), 1LL, std::multiplies<int64_t>());
  if (num > (int64_t)INT32_MAX) {
    MS_LOG(ERROR) << "Element number of tensor shouder be smaller than int32_max: " << num << " return INT32_MAX";
    return INT32_MAX;
  }
  return (int32_t)num;
}

int32_t Tensor::ElementsC4Num() const {
  if (this->category_ == CONST_SCALAR) {
    return 1;
  }
  int32_t result = 1;
  if (this->shape_.size() == 4) {
    result = Batch() * Height() * Width() * ((Channel() + 3) / 4 * 4);
  } else if (this->shape_.size() == 2) {
    result = this->shape_[0] * ((this->shape_[1] + 3) / 4 * 4);
  }
  return result;
}

int Tensor::DimensionSize(const size_t index) const {
  int dim_size = -1;
  if (index < shape_.size() && index >= 0) {
    dim_size = shape_[index];
  } else {
    MS_LOG(ERROR) << "Dimension index is wrong: " << index;
  }
  return dim_size;
}

std::string Tensor::ToString() const {
  std::ostringstream oss;
  oss << "schema::Format: " << EnumNameFormat(static_cast<schema::Format>(this->format_));
  oss << " DataType: " << this->data_type_;
  oss << " Category: " << this->category_;
  oss << " Shape:";
  for (auto &dim : this->shape()) {
    oss << " " << dim;
  }
  oss << std::endl << "Data:";
  switch (this->data_type_) {
    case kNumberTypeFloat32: {
      oss << DataToString<float>(this->data_c(), this->ElementsNum());
    } break;
    case kNumberTypeFloat16: {
      oss << DataToString<int16_t>(this->data_c(), this->ElementsNum());
    } break;
    case kNumberTypeInt32: {
      oss << DataToString<int32_t>(this->data_c(), this->ElementsNum());
    } break;
    case kNumberTypeInt16: {
      oss << DataToString<int16_t>(this->data_c(), this->ElementsNum());
    } break;
    case kNumberTypeInt8: {
      oss << DataToString<int8_t>(this->data_c(), this->ElementsNum());
    } break;
    default:
      oss << "Unsupported data type to print";
      break;
  }
  return oss.str();
}

void Tensor::set_root_tensor(Tensor *tensor) {
  this->root_tensor_ = tensor;
  if (this->root_tensor_ == this) {
    return;
  }
  if (this->root_tensor_ == nullptr) {
    return;
  }
  this->shape_ = this->root_tensor_->shape_;
  this->format_ = this->root_tensor_->format_;
  this->data_type_ = this->root_tensor_->data_type_;
  this->category_ = this->root_tensor_->category_;
  this->quant_params_ = this->root_tensor_->quant_params_;
  this->quant_clusters_ = this->root_tensor_->quant_clusters_;
}

int Tensor::MallocData(const AllocatorPtr allocator) {
  if (this->data_ != nullptr) {
    return RET_OK;
  }
  if (allocator != nullptr) {
    allocator_ = allocator;
  }
  auto data_size = this->Size();

  if (data_size > kMaxMallocSize) {
    MS_LOG(ERROR) << "Malloc size is too big while coping data, " << data_size << " bytes";
    return RET_ERROR;
  }
  if (allocator_ == nullptr) {
    this->data_ = malloc(data_size);
  } else {
    this->data_ = allocator_->Malloc(data_size);
  }
  if (this->data_ == nullptr) {
    MS_LOG(ERROR) << "Malloc tensor data failed, size=" << data_size;
    return RET_ERROR;
  }
  this->own_data_ = true;
  return RET_OK;
}

void Tensor::FreeData() {
  if (this->data_ == nullptr) {
    return;
  }
  if (!this->own_data_) {
    return;
  }
  if (allocator_ == nullptr) {
    free(this->data_);
    this->data_ = nullptr;
  } else {
    allocator_->Free(this->data_);
    this->data_ = nullptr;
  }
}

void *Tensor::ReallocData() {
  if (this->data_ != nullptr) {
    FreeData();
  }
  return this->MutableData();
}

void *Tensor::MutableData() {
  if (this->root_tensor_ != nullptr) {
    if (this->root_tensor_ != this && this->root_tensor_->data_ == nullptr) {
      MS_LOG(ERROR) << "root tensor has not been malloced";
      return nullptr;
    } else if (this->root_tensor_ != this && this->root_tensor_->data_ != nullptr) {
      return this->root_tensor_->data_;
    } else {
      // malloc self
    }
  }
  if (this->data_ == nullptr) {
    auto ret = this->MallocData();
    if (ret != 0) {
      MS_LOG(WARNING) << "Malloc data failed";
    }
  }
  Prepare();
  return this->data_;
}

void Tensor::IncRefCount() {
  ref_count_++;
  if (allocator_ != nullptr) {
    allocator_->IncRefCount(this->data_, 1);
  }
}

void Tensor::DecRefCount() {
  if (this->IsConst() || this->IsGraphInput()) {
    return;
  }
  int tensor_ref_count = --ref_count_;
  int data_ref_count = tensor_ref_count;

  if (allocator_ != nullptr) {
    data_ref_count = allocator_->DecRefCount(this->data_, 1);
  }
  if (tensor_ref_count <= 0) {
    if (data_ref_count <= 0) {
      FreeData();
    } else {
      data_ = nullptr;
    }
  }
}

void Tensor::AddQuantParam(const QuantArg &quant_arg) { this->quant_params_.push_back(quant_arg); }

std::vector<QuantArg> Tensor::quant_params() const { return this->quant_params_; }

std::vector<float> Tensor::quant_clusters() const { return this->quant_clusters_; }

void Tensor::set_quant_clusters(const std::vector<float> &clusters) { this->quant_clusters_ = clusters; }

std::vector<tensor::MSTensor *> TensorVectorCast(const std::vector<Tensor *> &src) {
  std::vector<tensor::MSTensor *> target(src.size());
  std::transform(src.begin(), src.end(), target.begin(), [](Tensor *t) { return static_cast<tensor::MSTensor *>(t); });
  return target;
}

}  // namespace lite

}  // namespace mindspore
