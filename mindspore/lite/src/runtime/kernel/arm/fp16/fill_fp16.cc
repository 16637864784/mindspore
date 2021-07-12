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

#include "src/runtime/kernel/arm/fp16/fill_fp16.h"
#include <vector>
#include "schema/model_generated.h"
#include "src/kernel_registry.h"
#include "include/errorcode.h"

using mindspore::kernel::KERNEL_ARCH;
using mindspore::lite::KernelRegistrar;
using mindspore::lite::RET_ERROR;
using mindspore::lite::RET_OK;
using mindspore::schema::PrimitiveType_Fill;

namespace mindspore::kernel {
int FillFp16CPUKernel::Init() {
  if (!InferShapeDone()) {
    return RET_OK;
  }
  return ReSize();
}

int FillFp16CPUKernel::ReSize() {
  data_size_ = out_tensors_.front()->ElementsNum();
  thread_sz_count_ = MSMIN(thread_count_, data_size_);
  if (thread_sz_count_ != 0) {
    thread_sz_stride_ = UP_DIV(data_size_, thread_sz_count_);
  }
  return RET_OK;
}

int FillFp16CPUKernel::DoFill(int task_id) {
  int size = MSMIN(thread_sz_stride_, data_size_ - task_id * thread_sz_stride_);
  if (size <= 0) {
    return RET_OK;
  }
  int offset = task_id * thread_sz_stride_;
  auto input_tensor = in_tensors_.at(0);
  int ret = RET_OK;
  if (input_tensor->data_type() == kNumberTypeFloat16) {
    ret = FillFp16(fp16_out_ptr_ + offset, size, fp16_src_data_);
  } else {
    return RET_ERROR;
  }
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "FillRun error task_id[" << task_id << "] error_code[" << ret << "]";
    return ret;
  }
  return RET_OK;
}

int FillRunFp16(void *cdata, int task_id, float lhs_scale, float rhs_scale) {
  auto g_kernel = reinterpret_cast<FillFp16CPUKernel *>(cdata);
  auto ret = g_kernel->DoFill(task_id);
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "FillRun error task_id[" << task_id << "] error_code[" << ret << "]";
    return ret;
  }
  return RET_OK;
}

int FillFp16CPUKernel::Run() {
  auto fill_input = in_tensors_.front();
  auto output = out_tensors_.front();
  if (fill_input->data_type() == kNumberTypeFloat16) {
    auto fill_data = reinterpret_cast<float16_t *>(fill_input->MutableData());
    fp16_src_data_ = fill_data[0];
    fp16_out_ptr_ = reinterpret_cast<float16_t *>(output->MutableData());
  } else {
    MS_LOG(ERROR) << "unsupported fill data type " << fill_input->data_type();
    return RET_ERROR;
  }
  auto ret = ParallelLaunch(this->ms_context_, FillRunFp16, this, thread_sz_count_);
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "FillRun error error_code[" << ret << "]";
    return ret;
  }
  return RET_OK;
}

REG_KERNEL(kCPU, kNumberTypeFloat16, PrimitiveType_Fill, LiteKernelCreator<FillFp16CPUKernel>)
}  // namespace mindspore::kernel
