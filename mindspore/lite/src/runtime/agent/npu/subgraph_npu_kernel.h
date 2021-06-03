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

#ifndef MINDSPORE_LITE_SRC_RUNTIME_AGENT_SUBGRAPH_NPU_KERNEL_H_
#define MINDSPORE_LITE_SRC_RUNTIME_AGENT_SUBGRAPH_NPU_KERNEL_H_
#include <vector>
#include <string>
#include <memory>
#include "include/hiai_ir_build.h"
#include "src/sub_graph_kernel.h"
#include "src/runtime/agent/npu/npu_executor.h"
#include "include/graph/op/all_ops.h"
#ifdef SUPPORT_NPU
#include "src/runtime/agent/npu/npu_manager.h"
#endif

namespace mindspore::kernel {
using mindspore::lite::RET_ERROR;
using mindspore::lite::RET_OK;
class SubGraphNpuKernel : public SubGraphKernel {
 public:
  SubGraphNpuKernel(const std::vector<kernel::LiteKernel *> &inKernels,
                    const std::vector<kernel::LiteKernel *> &outKernels, const std::vector<kernel::LiteKernel *> &nodes,
                    Kernel *kernel, lite::NPUManager *npu_manager = nullptr)
      : SubGraphKernel(inKernels, outKernels, nodes, kernel), npu_manager_(npu_manager) {
    subgraph_type_ = kNpuSubGraph;
    desc_.arch = kernel::KERNEL_ARCH::kNPU;
  }

  ~SubGraphNpuKernel() override;

  int Init() override;

  int Prepare() override;

  int Execute() override;

  int Execute(const KernelCallBack &before, const KernelCallBack &after) override { return this->Execute(); }

  int ReSize() override {
    MS_LOG(ERROR) << "NPU does not support the resize function temporarily.";
    return RET_ERROR;
  }

  void set_out_tensor(lite::Tensor *out_tensor, int index) override {
    MS_ASSERT(index < out_tensor_sorted_.size());
    auto src_output_tensor = out_tensors()[index];
    LiteKernel::set_out_tensor(out_tensor, index);
    for (int i = 0; i < out_tensor_sorted_.size(); i++) {
      if (out_tensor_sorted_[i] == src_output_tensor) {
        out_tensor_sorted_[i] = out_tensor;
      }
    }
  }

 private:
  std::shared_ptr<domi::ModelBufferData> BuildIRModel();

  int BuildNPUInputOp();

  int BuildNPUOutputOp();

  std::vector<ge::Operator> GetNPUNodes(const std::vector<kernel::LiteKernel *> &nodes);

  bool IsSubGraphInputTensor(lite::Tensor *inputs);

  std::string GetOMModelName();

 private:
  bool is_compiled_ = false;

  lite::NPUManager *npu_manager_ = nullptr;

  std::vector<ge::Operator> subgraph_input_op_;

  std::vector<ge::Operator> subgraph_output_op_;

  std::vector<lite::Tensor *> out_tensor_sorted_;

  std::vector<ge::Operator *> op_buffer_;
};
}  // namespace mindspore::kernel
#endif  // MINDSPORE_LITE_SRC_RUNTIME_AGENT_SUBGRAPH_NPU_KERNEL_H_
