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
#include <random>
#include <thread>
#include "runtime/device/cpu/cpu_device_address.h"
#include "backend/kernel_compiler/cpu/random_cpu_kernel.h"

namespace mindspore {
namespace kernel {
void StandardNormal(float *output, std::normal_distribution<float> distribution,
                    std::default_random_engine random_generator, size_t start, size_t end) {
  for (size_t i = start; i < end; i++) {
    output[i] = distribution(random_generator);
  }
}

void LaunchStandardNormal(int seed, int seed2, const std::vector<AddressPtr> &outputs) {
  unsigned int RNG_seed;
  std::random_device rd;
  if (seed2 != 0) {
    RNG_seed = IntToUint(seed2);
  } else if (seed != 0) {
    RNG_seed = IntToUint(seed);
  } else {
    RNG_seed = rd();
  }

  auto output = reinterpret_cast<float *>(outputs[0]->addr);
  size_t lens = outputs[0]->size / sizeof(float);
  std::normal_distribution<float> distribution;
  auto task = [&](size_t start, size_t end) {
    std::default_random_engine random_generator(++RNG_seed);
    StandardNormal(output, distribution, random_generator, start, end);
  };
  CPUKernelUtils::ParallelFor(task, lens);
}

void RandomCPUKernel::InitKernel(const CNodePtr &kernel_node) {
  MS_EXCEPTION_IF_NULL(kernel_node);
  std::string kernel_name = AnfAlgo::GetCNodeName(kernel_node);
  auto iter = kRandomOpTypeMap.find(kernel_name);
  if (iter == kRandomOpTypeMap.end()) {
    MS_LOG(EXCEPTION) << "Random operation " << kernel_name << " is not supported.";
  } else {
    random_op_type_ = iter->second;
  }

  size_t input_num = AnfAlgo::GetInputTensorNum(kernel_node);
  if ((random_op_type_ == RANDOM_OP_NORMAL) && input_num != 1) {
    MS_LOG(EXCEPTION) << "Input number is " << input_num << ", but random op needs 1 input.";
  }

  size_t output_num = AnfAlgo::GetOutputTensorNum(kernel_node);
  if (output_num != 1) {
    MS_LOG(EXCEPTION) << "Output number is " << output_num << ", but random op needs 1 output.";
  }

  seed_ = LongToInt(GetValue<int64_t>(AnfAlgo::GetCNodePrimitive(kernel_node)->GetAttr("seed")));
  seed2_ = LongToInt(GetValue<int64_t>(AnfAlgo::GetCNodePrimitive(kernel_node)->GetAttr("seed2")));
}

bool RandomCPUKernel::Launch(const std::vector<kernel::AddressPtr> &inputs, const std::vector<kernel::AddressPtr> &,
                             const std::vector<kernel::AddressPtr> &outputs) {
  switch (random_op_type_) {
    case RANDOM_OP_NORMAL: {
      LaunchStandardNormal(seed_, seed2_, outputs);
      break;
    }
    default: {
      MS_LOG(EXCEPTION) << "Random operation " << random_op_type_ << " is not supported.";
    }
  }
  return true;
}
}  // namespace kernel
}  // namespace mindspore
