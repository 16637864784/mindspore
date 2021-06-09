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
#include "src/runtime/kernel/arm/base/split_with_over_lap_base.h"
#include "schema/model_generated.h"
#include "src/kernel_registry.h"
#include "src/tensor.h"

using mindspore::kernel::KERNEL_ARCH;
using mindspore::lite::KernelRegistrar;
using mindspore::lite::RET_ERROR;
using mindspore::lite::RET_OK;
using mindspore::schema::PrimitiveType_SplitWithOverlap;

namespace mindspore::kernel {

void SplitWithOverlapBaseCPUKernel::CalculateSplitedShapes(const SplitWithOverlapParameter *param,
                                                           const std::vector<int> &shape) {
  int total_block_count = 0;
  for (auto i = 0; i < param->num_split_; i++) {
    total_block_count += param->ratio_[i];
  }

  auto split_dim_size = shape[param->split_dim_];

  std::vector<int> borders;
  borders.emplace_back(0);
  int visited_block = 0;
  for (auto i = 0; i < param->num_split_ - 1; i++) {
    visited_block += param->ratio_[i];
    auto cur_border = UP_DIV(split_dim_size * visited_block, total_block_count);
    if (param->split_stride_ != 0) {
      // make sure border align with stride
      cur_border = UP_ROUND(cur_border + param->pad_top_, param->split_stride_);
      borders.emplace_back(cur_border - param->pad_top_);
    } else {
      borders.emplace_back(cur_border);
    }
  }
  borders.emplace_back(split_dim_size);

  for (auto i = 0; i < param->num_split_; i++) {
    start_indices_.emplace_back(borders[i]);
    end_indices_.emplace_back(borders[i + 1]);

    // overlap: calibrate start_indices and end_indices by adding extends
    start_indices_[i] -= param->extend_top_[i];
    end_indices_[i] += param->extend_bottom_[i];
  }
}

int SplitWithOverlapBaseCPUKernel::Init() { return RET_OK; }

int SplitWithOverlapBaseCPUKernel::ReSize() { return RET_OK; }

int SplitWithOverlapBaseCPUKernel::Split(int task_id) {
  DoSplitWithOverlap(input_ptr_, output_ptr_.data(), param_->num_split_, split_dim_size_, element_bytes_,
                     outer_total_dim_, inner_stride_, start_indices_.data(), end_indices_.data());

  return RET_OK;
}

int SplitWithOverlapRun(void *cdata, int task_id, float lhs_scale, float rhs_scale) {
  auto g_kernel = reinterpret_cast<SplitWithOverlapBaseCPUKernel *>(cdata);
  auto ret = g_kernel->Split(task_id);
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "SplitWithOverlapRun error task_id[" << task_id << "] error_code[" << ret << "]";
    return RET_ERROR;
  }

  return RET_OK;
}

int SplitWithOverlapBaseCPUKernel::Run() {
  auto prepare_ret = Prepare();
  if (prepare_ret != RET_OK) {
    MS_LOG(ERROR) << "Prepare fail! ret: " << prepare_ret;
    return prepare_ret;
  }

  auto in_tensor = in_tensors_.front();
  input_ptr_ = reinterpret_cast<char *>(in_tensor->data_c());
  auto input_shape = in_tensor->shape();

  start_indices_.clear();
  end_indices_.clear();
  output_ptr_.clear();

  for (int i = 0; i < param_->num_split_; i++) {
    output_ptr_.push_back(reinterpret_cast<char *>(out_tensors_.at(i)->data_c()));
  }

  CalculateSplitedShapes(param_, input_shape);

  outer_total_dim_ = 1;
  inner_stride_ = 1;
  split_dim_size_ = input_shape[param_->split_dim_];
  element_bytes_ = static_cast<int>(lite::DataTypeSize(in_tensor->data_type()));

  for (auto i = 0; i < param_->split_dim_; i++) {
    outer_total_dim_ *= input_shape[i];
  }

  for (int i = static_cast<int>(input_shape.size()) - 1; i > param_->split_dim_; i--) {
    inner_stride_ *= input_shape[i];
  }

  auto ret = static_cast<const lite::InnerContext *>(this->context_)
               ->thread_pool_->ParallelLaunch(SplitWithOverlapRun, this, context_->thread_num_);
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "ParallelLaunch for SplitWIthOverlapRun run fail. errorcode:[" << ret << "]";
    return RET_ERROR;
  }

  return RET_OK;
}

REG_KERNEL(kCPU, kNumberTypeFloat32, PrimitiveType_SplitWithOverlap, LiteKernelCreator<SplitWithOverlapBaseCPUKernel>)
REG_KERNEL(kCPU, kNumberTypeFloat16, PrimitiveType_SplitWithOverlap, LiteKernelCreator<SplitWithOverlapBaseCPUKernel>)
REG_KERNEL(kCPU, kNumberTypeInt8, PrimitiveType_SplitWithOverlap, LiteKernelCreator<SplitWithOverlapBaseCPUKernel>)
}  // namespace mindspore::kernel
