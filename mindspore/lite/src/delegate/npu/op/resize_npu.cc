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

#include "src/delegate/npu/op/resize_npu.h"
#include <memory>
#include "src/delegate/npu/npu_converter_utils.h"

namespace mindspore {
int ResizeNPUOp::IsSupport(const schema::Primitive *primitive, const std::vector<mindspore::MSTensor> &in_tensors,
                           const std::vector<mindspore::MSTensor> &out_tensors) {
  auto resize_prim = primitive->value_as_Resize();
  if (resize_prim == nullptr) {
    MS_LOG(ERROR) << "Get null primitive value for op ." << name_;
    return RET_ERROR;
  }
  resize_method_ = resize_prim->method();
  if (resize_method_ != schema::ResizeMethod_LINEAR && resize_method_ != schema::ResizeMethod_NEAREST) {
    MS_LOG(WARNING) << "Unsupported resize method type: " << resize_method_;
    return RET_NOT_SUPPORT;
  }

  if (in_tensors[0].Shape()[1] > out_tensors[0].Shape()[1] || in_tensors[0].Shape()[2] > out_tensors[0].Shape()[2]) {
    MS_LOG(WARNING) << "Npu resize does not support reduction.";
    return RET_NOT_SUPPORT;
  }
  return RET_OK;
}

int ResizeNPUOp::Init(const schema::Primitive *primitive, const std::vector<mindspore::MSTensor> &in_tensors,
                      const std::vector<mindspore::MSTensor> &out_tensors) {
  auto resize_prim = primitive->value_as_Resize();
  if (resize_prim == nullptr) {
    MS_LOG(ERROR) << "Get null primitive value for op ." << name_;
    return RET_ERROR;
  }
  if (in_tensors.size() == 1) {
    new_height_ = resize_prim->new_height();
    new_width_ = resize_prim->new_width();
  } else if (in_tensors.size() == 2) {
    auto out_size = in_tensors.at(1).Data();
    if (out_size == nullptr) {
      MS_LOG(ERROR) << "Out size is not assigned";
      return RET_ERROR;
    }
    new_height_ = out_tensors.at(0).Shape().at(1);
    new_width_ = out_tensors.at(0).Shape().at(2);
  } else {
    MS_LOG(ERROR) << "Get resize op new_height and new_width error.";
    return RET_ERROR;
  }

  ge::TensorDesc sizeTensorDesc(ge::Shape({2}), ge::FORMAT_NCHW, ge::DT_INT32);
  ge::TensorPtr sizeTensor = std::make_shared<hiai::Tensor>(sizeTensorDesc);
  vector<int32_t> dataValue = {static_cast<int32_t>(new_height_), static_cast<int32_t>(new_width_)};
  sizeTensor->SetData(reinterpret_cast<uint8_t *>(dataValue.data()), 2 * sizeof(int32_t));
  out_size_ = new (std::nothrow) hiai::op::Const(name_ + "_size");
  out_size_->set_attr_value(sizeTensor);

  if (resize_method_ == schema::ResizeMethod_LINEAR) {
    auto resize_bilinear = new (std::nothrow) hiai::op::ResizeBilinearV2(name_);
    if (resize_bilinear == nullptr) {
      MS_LOG(ERROR) << " resize_ is nullptr.";
      return RET_ERROR;
    }
    resize_bilinear->set_attr_align_corners(resize_prim->coordinate_transform_mode() ==
                                            schema::CoordinateTransformMode_ALIGN_CORNERS);
    resize_bilinear->set_input_size(*out_size_);
    resize_bilinear->set_attr_half_pixel_centers(resize_prim->preserve_aspect_ratio());
    resize_ = resize_bilinear;
  } else if (resize_method_ == schema::ResizeMethod_NEAREST) {
    auto resize_nearest = new (std::nothrow) hiai::op::ResizeNearestNeighborV2(name_);
    if (resize_nearest == nullptr) {
      MS_LOG(ERROR) << " resize_ is nullptr.";
      return RET_ERROR;
    }
    resize_nearest->set_attr_align_corners(resize_prim->coordinate_transform_mode() ==
                                           schema::CoordinateTransformMode_ALIGN_CORNERS);
    resize_nearest->set_input_size(*out_size_);
  } else {
    MS_LOG(WARNING) << "Unsupported resize method type:" << resize_method_;
    return RET_ERROR;
  }
  return RET_OK;
}

int ResizeNPUOp::SetNPUInputs(const std::vector<mindspore::MSTensor> &in_tensors,
                              const std::vector<mindspore::MSTensor> &out_tensors,
                              const std::vector<ge::Operator *> &npu_inputs) {
  if (resize_method_ == schema::ResizeMethod_LINEAR) {
    auto resize_bilinear = reinterpret_cast<hiai::op::ResizeBilinearV2 *>(resize_);
    resize_bilinear->set_input_x(*npu_inputs[0]);
  } else if (resize_method_ == schema::ResizeMethod_NEAREST) {
    auto resize_nearest = reinterpret_cast<hiai::op::ResizeNearestNeighborV2 *>(resize_);
    resize_nearest->set_input_x(*npu_inputs[0]);
  } else {
    MS_LOG(WARNING) << "Unsupported resize method type:" << resize_method_;
    return RET_ERROR;
  }
  return RET_OK;
}

ge::Operator *ResizeNPUOp::GetNPUOp() { return this->resize_; }

ResizeNPUOp::~ResizeNPUOp() {
  if (resize_ != nullptr) {
    delete resize_;
    resize_ = nullptr;
  }
  if (out_size_ != nullptr) {
    delete out_size_;
    out_size_ = nullptr;
  }
}
}  // namespace mindspore
