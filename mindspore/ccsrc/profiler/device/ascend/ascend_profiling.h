/**
 * Copyright 2021 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef MINDSPORE_CCSRC_PROFILER_DEVICE_ASCEND_PROFILING_H
#define MINDSPORE_CCSRC_PROFILER_DEVICE_ASCEND_PROFILING_H
#include <string>
#include <memory>
#include <map>
#include "profiler/device/profiling.h"
#include "acl/acl_prof.h"
#include "backend/common/session/kernel_graph.h"
#include "kernel/kernel.h"

namespace mindspore {
namespace profiler {
namespace ascend {
class AscendProfiler : public Profiler {
 public:
  static std::shared_ptr<AscendProfiler> &GetInstance();
  AscendProfiler() : profiling_options_("") {}
  ~AscendProfiler() = default;
  AscendProfiler(const AscendProfiler &) = delete;
  AscendProfiler &operator=(const AscendProfiler &) = delete;
  void Init(const std::string &profileDataPath) { return; }
  void InitProfiling(const std::string &profiling_path, uint32_t device_id, const std::string &profiling_options);
  void Stop();
  void StepProfilingEnable(const bool enable_flag) override;
  void OpDataProducerEnd() { return; }
  void Start();
  bool GetProfilingEnableFlag() const { return enable_flag_; }
  std::string GetProfilingOptions() const { return profiling_options_; }
  uint64_t GetOptionsMask() const;
  aclprofAicoreMetrics GetAicMetrics() const;
  void Finalize() const;
  bool IsInitialized() const { return init_flag_; }
  void ReportErrorMessage() const;
  void GetNodeTaskIdStreamId(const CNodePtr &kernel, uint32_t graph_id, int device_id, const KernelType kernel_type);
  bool GetNetDynamicShapeStatus() const { return is_dynamic_shape_net_; }
  void SetNetDynamicShapeStatus() { is_dynamic_shape_net_ = true; }
  std::map<std::thread::id, uint32_t> last_tid;
  std::map<std::thread::id, uint32_t> last_streamid;

 protected:
  void SaveProfileData() { return; }
  void ClearInst() { return; }

 private:
  static std::shared_ptr<AscendProfiler> ascend_profiler_;
  bool is_dynamic_shape_net_ = 0;
  std::string profiling_options_;
  uint32_t device_id_ = 0;
  uint32_t aicpu_kernel_type_ = 2;
  uint32_t max_op_taskid_limit_ = 65536;
  aclprofConfig *acl_config_{nullptr};
};
}  // namespace ascend
}  // namespace profiler
}  // namespace mindspore
#endif
