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
#ifndef MINDSPORE_CCSRC_BACKEND_KERNEL_COMPILER_TBE_ASCEND_KERNEL_COMPILE_H_
#define MINDSPORE_CCSRC_BACKEND_KERNEL_COMPILER_TBE_ASCEND_KERNEL_COMPILE_H_
#include <string>
#include <map>
#include <tuple>
#include <set>
#include <memory>
#include <vector>
#include <utility>
#include "ir/anf.h"
#include "backend/kernel_compiler/kernel.h"
#include "backend/kernel_compiler/kernel_fusion.h"
#include "backend/kernel_compiler/tbe/tbe_kernel_build.h"
#include "backend/kernel_compiler/tbe/tbe_kernel_parallel_build.h"
#include "backend/session/kernel_graph.h"

namespace mindspore {
namespace kernel {
namespace ascend {
using KernelModMap = std::map<int64_t, KernelModPtr>;
struct TargetJobStatus {
  int target_job_id;
  std::string job_status;
};

class AscendKernelCompileManager {
 public:
  AscendKernelCompileManager() = default;
  ~AscendKernelCompileManager();
  static std::shared_ptr<AscendKernelCompileManager> GetInstance() {
    static auto instance = std::make_shared<AscendKernelCompileManager>();
    if (!instance->tbe_init_flag_) {
      instance->TbeInitialize();
    }
    return instance;
  }
  void TbeInitialize();
  void TbeFinalize();
  // kernel select
  std::string AscendOpSelectFormat(const AnfNodePtr &node);
  bool AscendOpCheckSupported(const AnfNodePtr &node);
  // pre build
  void AscendPreBuild(const std::shared_ptr<session::KernelGraph> &kernel_graph);
  // single op compile
  bool AscendSingleOpCompile(const std::vector<AnfNodePtr> &anf_nodes);
  // fusion op compile
  KernelModMap AscendFusionOpCompile(const std::vector<FusionScopeInfo> &fusion_scopes);
  // clear prev job's cache
  void ResetOldTask();

 private:
  void GetAllAscendNodes(const std::shared_ptr<session::KernelGraph> &kernel_graph, std::vector<AnfNodePtr> *tbe_nodes);
  void QueryFinishJob(const std::string &type);
  void ParseTargetJobStatus(const std::string &type, const std::string &build_res, std::vector<int> *success_job);
  void QueryPreBuildFinishJob();
  void QueryFusionFinishJob(KernelModMap *kernel_mode_ret);
  void PrintProcessLog(const nlohmann::json &json, int adjust_log_level);
  bool JsonAssemble(const std::string &job_type, const nlohmann::json &src_json, nlohmann::json *dst_json);
  void PrintInitResult(const nlohmann::json &json);
  void PrintSingleBuildResult(const nlohmann::json &json);
  void PrintFusionOpBuildResult(const nlohmann::json &json);
  std::string FormatSelectResultProcess(const nlohmann::json &json);
  void QueryResultProcess(const nlohmann::json &json, TargetJobStatus *task_info, int adjust_log_level);
  nlohmann::json TurnStrToJson(const std::string &str);

  static bool tbe_init_flag_;
  static bool is_tune_flag_;
  std::shared_ptr<ParallelBuildManager> build_manager_ = nullptr;
  std::map<int, nlohmann::json> job_list_;
  std::map<int, std::string> fusion_op_names_;
};
}  // namespace ascend
}  // namespace kernel
}  // namespace mindspore

#endif  // MINDSPORE_CCSRC_BACKEND_KERNEL_COMPILER_TBE_ASCEND_KERNEL_COMPILE_H_
