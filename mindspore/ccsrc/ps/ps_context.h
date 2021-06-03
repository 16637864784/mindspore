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

#ifndef MINDSPORE_CCSRC_PS_CONTEXT_H_
#define MINDSPORE_CCSRC_PS_CONTEXT_H_

#include <map>
#include <string>
#include <memory>
#include "ps/constants.h"
#include "ps/core/cluster_metadata.h"
#include "ps/core/cluster_config.h"

namespace mindspore {
namespace ps {
constexpr char kServerModePS[] = "PARAMETER_SERVER";
constexpr char kServerModeFL[] = "FEDERATED_LEARNING";
constexpr char kServerModeHybrid[] = "HYBRID_TRAINING";
constexpr char kEnvRole[] = "MS_ROLE";
constexpr char kEnvRoleOfPServer[] = "MS_PSERVER";
constexpr char kEnvRoleOfServer[] = "MS_SERVER";
constexpr char kEnvRoleOfWorker[] = "MS_WORKER";
constexpr char kEnvRoleOfScheduler[] = "MS_SCHED";
constexpr char kEnvRoleOfNotPS[] = "MS_NOT_PS";

// Use binary data to represent federated learning server's context so that we can judge which round resets the
// iteration. From right to left, each bit stands for:
// 0: Server is in parameter server mode.
// 1: Server is in federated learning mode.
// 2: Server is in mixed training mode.
// 3: Server enables sucure aggregation.
// For example: 1010 stands for that the server is in federated learning mode and sucure aggregation  is enabled.
enum class ResetterRound { kNoNeedToReset, kUpdateModel, kReconstructSeccrets, kPushWeight };
const std::map<uint32_t, ResetterRound> kServerContextToResetRoundMap = {{0b0010, ResetterRound::kUpdateModel},
                                                                         {0b1010, ResetterRound::kReconstructSeccrets},
                                                                         {0b1100, ResetterRound::kPushWeight},
                                                                         {0b0100, ResetterRound::kPushWeight},
                                                                         {0b0100, ResetterRound::kUpdateModel}};

class PSContext {
 public:
  ~PSContext() = default;
  PSContext(PSContext const &) = delete;
  PSContext &operator=(const PSContext &) = delete;
  static std::shared_ptr<PSContext> instance();

  void SetPSEnable(bool enabled);
  bool is_ps_mode() const;
  void Reset();
  std::string ms_role() const;
  bool is_worker() const;
  bool is_server() const;
  bool is_scheduler() const;
  uint32_t initial_worker_num() const;
  uint32_t initial_server_num() const;
  std::string scheduler_host() const;
  void SetPSRankId(int rank_id);
  int ps_rank_id() const;
  void InsertHashTableSize(const std::string &param_name, size_t cache_vocab_size, size_t embedding_size,
                           size_t vocab_size) const;
  void ReInsertHashTableSize(const std::string &new_param_name, const std::string &cur_param_name,
                             size_t cache_vocab_size, size_t embedding_size) const;
  void InsertWeightInitInfo(const std::string &param_name, size_t global_seed, size_t op_seed) const;
  void InsertAccumuInitInfo(const std::string &param_name, float init_val) const;
  void CloneHashTable(const std::string &dest_param_name, const std::string &src_param_name) const;
  void set_cache_enable(bool cache_enable) const;
  void set_rank_id(int rank_id) const;
  bool enable_ssl() const;
  void set_enable_ssl(bool enabled);

  // In new server framework, process role, worker number, server number, scheduler ip and scheduler port should be set
  // by ps_context.
  void set_server_mode(const std::string &server_mode);
  const std::string &server_mode() const;

  void set_ms_role(const std::string &role);

  void set_worker_num(uint32_t worker_num);
  uint32_t worker_num() const;

  void set_server_num(uint32_t server_num);
  uint32_t server_num() const;

  void set_scheduler_ip(const std::string &sched_ip);
  std::string scheduler_ip() const;

  void set_scheduler_port(uint16_t sched_port);
  uint16_t scheduler_port() const;

  // Methods federated learning.

  // Generate which round should reset the iteration.
  void GenerateResetterRound();
  ResetterRound resetter_round() const;

  void set_fl_server_port(uint16_t fl_server_port);
  uint16_t fl_server_port() const;

  // Set true if this process is a federated learning worker in cross-silo scenario.
  void set_fl_client_enable(bool enabled);
  bool fl_client_enable();

  void set_start_fl_job_threshold(uint64_t start_fl_job_threshold);
  uint64_t start_fl_job_threshold() const;

  void set_start_fl_job_time_window(uint64_t start_fl_job_time_window);
  uint64_t start_fl_job_time_window() const;

  void set_update_model_ratio(float update_model_ratio);
  float update_model_ratio() const;

  void set_update_model_time_window(uint64_t update_model_time_window);
  uint64_t update_model_time_window() const;

  void set_fl_name(const std::string &fl_name);
  const std::string &fl_name() const;

  // Set the iteration number of the federated learning.
  void set_fl_iteration_num(uint64_t fl_iteration_num);
  uint64_t fl_iteration_num() const;

  // Set the training epoch number of the client.
  void set_client_epoch_num(uint64_t client_epoch_num);
  uint64_t client_epoch_num() const;

  // Set the data batch size of the client.
  void set_client_batch_size(uint64_t client_batch_size);
  uint64_t client_batch_size() const;

  void set_client_learning_rate(float client_learning_rate);
  float client_learning_rate() const;

  core::ClusterConfig &cluster_config();

  void set_scheduler_manage_port(uint16_t sched_port);
  uint16_t scheduler_manage_port() const;

 private:
  PSContext()
      : ps_enabled_(false),
        is_worker_(false),
        is_pserver_(false),
        is_sched_(false),
        enable_ssl_(false),
        rank_id_(-1),
        worker_num_(0),
        server_num_(0),
        scheduler_host_(""),
        scheduler_port_(0),
        role_(kEnvRoleOfNotPS),
        server_mode_(""),
        resetter_round_(ResetterRound::kNoNeedToReset),
        fl_server_port_(0),
        fl_client_enable_(false),
        fl_name_(""),
        start_fl_job_threshold_(0),
        start_fl_job_time_window_(3000),
        update_model_ratio_(1.0),
        update_model_time_window_(3000),
        fl_iteration_num_(20),
        client_epoch_num_(25),
        client_batch_size_(32),
        client_learning_rate_(0.001),
        secure_aggregation_(false),
        cluster_config_(nullptr),
        scheduler_manage_port_(0) {}
  bool ps_enabled_;
  bool is_worker_;
  bool is_pserver_;
  bool is_sched_;
  bool enable_ssl_;
  int rank_id_;
  uint32_t worker_num_;
  uint32_t server_num_;
  std::string scheduler_host_;
  uint16_t scheduler_port_;

  // The server process's role.
  std::string role_;

  // Server mode which could be Parameter Server, Federated Learning and Hybrid Training mode.
  std::string server_mode_;

  // The round which will reset the iteration. Used in federated learning for now.
  ResetterRound resetter_round_;

  // Http port of federated learning server.
  uint16_t fl_server_port_;

  // Whether this process is the federated client. Used in cross-silo scenario of federated learning.
  bool fl_client_enable_;

  // Federated learning job name.
  std::string fl_name_;

  // The threshold count of startFLJob round. Used in federated learning for now.
  uint64_t start_fl_job_threshold_;

  // The time window of startFLJob round in millisecond.
  uint64_t start_fl_job_time_window_;

  // Update model threshold is a certain ratio of start_fl_job threshold which is set as update_model_ratio_.
  float update_model_ratio_;

  // The time window of updateModel round in millisecond.
  uint64_t update_model_time_window_;

  // Iteration number of federeated learning, which is the number of interactions between client and server.
  uint64_t fl_iteration_num_;

  // Client training epoch number. Used in federated learning for now.
  uint64_t client_epoch_num_;

  // Client training data batch size. Used in federated learning for now.
  uint64_t client_batch_size_;

  // Client training learning rate. Used in federated learning for now.
  float client_learning_rate_;

  // Whether to use secure aggregation algorithm. Used in federated learning for now.
  bool secure_aggregation_;

  // The cluster config read through environment variables, the value does not change.
  std::unique_ptr<core::ClusterConfig> cluster_config_;

  // The port used by scheduler to receive http requests for scale out or scale in.
  uint16_t scheduler_manage_port_;
};
}  // namespace ps
}  // namespace mindspore
#endif  // MINDSPORE_CCSRC_PS_CONTEXT_H_
