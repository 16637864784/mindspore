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

#include "ps/ps_context.h"
#include "utils/log_adapter.h"
#include "utils/ms_utils.h"
#include "backend/kernel_compiler/kernel.h"
#if (ENABLE_CPU && !_WIN32)
#include "ps/ps_cache/ps_cache_manager.h"
#include "ps/ps_cache/ps_data/ps_data_prefetch.h"
#endif

namespace mindspore {
namespace ps {
std::shared_ptr<PSContext> PSContext::instance() {
  static std::shared_ptr<PSContext> ps_instance = nullptr;
  if (ps_instance == nullptr) {
    ps_instance.reset(new (std::nothrow) PSContext());
  }
  return ps_instance;
}

void PSContext::SetPSEnable(bool enabled) {
  ps_enabled_ = enabled;
  if (ps_enabled_) {
    std::string ms_role = common::GetEnv(kEnvRole);
    MS_LOG(INFO) << "PS mode is enabled. MS_ROLE is " << ms_role;
    if (ms_role == kEnvRoleOfWorker) {
      is_worker_ = true;
    } else if (ms_role == kEnvRoleOfPServer) {
      is_pserver_ = true;
    } else if (ms_role == kEnvRoleOfScheduler) {
      is_sched_ = true;
    } else {
      MS_LOG(WARNING) << "MS_ROLE is " << ms_role << ", which is invalid.";
    }

    worker_num_ = std::strtol(common::GetEnv(kEnvWorkerNum).c_str(), nullptr, 10);
    server_num_ = std::strtol(common::GetEnv(kEnvPServerNum).c_str(), nullptr, 10);
    scheduler_host_ = common::GetEnv(kEnvSchedulerHost);
    scheduler_port_ = std::strtol(common::GetEnv(kEnvSchedulerPort).c_str(), nullptr, 10);
    core::ClusterMetadata::instance()->Init(worker_num_, server_num_, scheduler_host_, scheduler_port_);
  } else {
    MS_LOG(INFO) << "PS mode is disabled.";
    is_worker_ = false;
    is_pserver_ = false;
    is_sched_ = false;
  }
}

bool PSContext::is_ps_mode() const {
  if (server_mode_ == kServerModeFL || server_mode_ == kServerModeHybrid) {
    return true;
  }
  return ps_enabled_;
}

void PSContext::Reset() {
  ps_enabled_ = false;
  is_worker_ = false;
  is_pserver_ = false;
  is_sched_ = false;
#if (ENABLE_CPU && !_WIN32)
  if (ps::PsDataPrefetch::GetInstance().cache_enable()) {
    ps_cache_instance.Finalize();
    set_cache_enable(false);
  }
#endif
}

std::string PSContext::ms_role() const {
  if (server_mode_ == kServerModeFL || server_mode_ == kServerModeHybrid) {
    return role_;
  }
  if (is_worker_) {
    return kEnvRoleOfWorker;
  } else if (is_pserver_) {
    return kEnvRoleOfPServer;
  } else if (is_sched_) {
    return kEnvRoleOfScheduler;
  } else {
    return kEnvRoleOfNotPS;
  }
}

bool PSContext::is_worker() const {
  if (server_mode_ == kServerModeFL || server_mode_ == kServerModeHybrid) {
    return role_ == kRoleOfWorker;
  }
  return is_worker_;
}

bool PSContext::is_server() const {
  if (server_mode_ == kServerModeFL || server_mode_ == kServerModeHybrid) {
    return role_ == kEnvRoleOfServer;
  }
  return is_pserver_;
}

bool PSContext::is_scheduler() const {
  if (server_mode_ == kServerModeFL || server_mode_ == kServerModeHybrid) {
    return role_ == kEnvRoleOfScheduler;
  }
  return is_sched_;
}

uint32_t PSContext::initial_worker_num() { return worker_num_; }

uint32_t PSContext::initial_server_num() { return server_num_; }

std::string PSContext::scheduler_host() { return scheduler_host_; }

uint16_t PSContext::scheduler_port() { return scheduler_port_; }

void PSContext::SetPSRankId(int rank_id) { rank_id_ = rank_id; }

int PSContext::ps_rank_id() const { return rank_id_; }

void PSContext::InsertHashTableSize(const std::string &param_name, size_t cache_vocab_size, size_t embedding_size,
                                    size_t vocab_size) const {
#if (ENABLE_CPU && !_WIN32)
  ps_cache_instance.InsertHashTableSize(param_name, cache_vocab_size, embedding_size, vocab_size);
#endif
}

void PSContext::ReInsertHashTableSize(const std::string &new_param_name, const std::string &cur_param_name,
                                      size_t cache_vocab_size, size_t embedding_size) const {
#if (ENABLE_CPU && !_WIN32)
  ps_cache_instance.ReInsertHashTableSize(new_param_name, cur_param_name, cache_vocab_size, embedding_size);
#endif
}

void PSContext::InsertWeightInitInfo(const std::string &param_name, size_t global_seed, size_t op_seed) const {
#if (ENABLE_CPU && !_WIN32)
  ps_cache_instance.InsertWeightInitInfo(param_name, global_seed, op_seed);
#endif
}

void PSContext::InsertAccumuInitInfo(const std::string &param_name, float init_val) const {
#if (ENABLE_CPU && !_WIN32)
  ps_cache_instance.InsertAccumuInitInfo(param_name, init_val);
#endif
}

void PSContext::CloneHashTable(const std::string &dest_param_name, const std::string &src_param_name) const {
#if (ENABLE_CPU && !_WIN32)
  ps_cache_instance.CloneHashTable(dest_param_name, src_param_name);
#endif
}

void PSContext::set_cache_enable(bool cache_enable) const {
#if (ENABLE_CPU && !_WIN32)
  PsDataPrefetch::GetInstance().set_cache_enable(cache_enable);
#endif
}

void PSContext::set_rank_id(int rank_id) const {
#if (ENABLE_CPU && !_WIN32)
  ps_cache_instance.set_rank_id(rank_id);
#endif
}

void PSContext::set_server_mode(const std::string &server_mode) {
  if (server_mode != kServerModePS && server_mode != kServerModeFL && server_mode != kServerModeHybrid) {
    MS_LOG(EXCEPTION) << server_mode << " is invalid. Server mode must be " << kServerModePS << " or " << kServerModeFL
                      << " or " << kServerModeHybrid;
    return;
  }
  server_mode_ = server_mode;
}

const std::string &PSContext::server_mode() const { return server_mode_; }

void PSContext::set_ms_role(const std::string &role) {
  if (server_mode_ != kServerModeFL && server_mode_ != kServerModeHybrid) {
    MS_LOG(EXCEPTION) << "Only federated learning supports to set role by ps context.";
    return;
  }
  if (role != kEnvRoleOfWorker && role != kEnvRoleOfServer && role != kEnvRoleOfScheduler) {
    MS_LOG(EXCEPTION) << "ms_role " << role << " is invalid.";
    return;
  }
  role_ = role;
}

void PSContext::set_worker_num(uint32_t worker_num) { worker_num_ = worker_num; }
uint32_t PSContext::worker_num() const { return worker_num_; }

void PSContext::set_server_num(uint32_t server_num) {
  if (server_num == 0) {
    MS_LOG(EXCEPTION) << "Server number must be greater than 0.";
    return;
  }
  server_num_ = server_num;
}
uint32_t PSContext::server_num() const { return server_num_; }

void PSContext::set_scheduler_ip(const std::string &sched_ip) { scheduler_host_ = sched_ip; }

std::string PSContext::scheduler_ip() const { return scheduler_host_; }

void PSContext::set_scheduler_port(uint16_t sched_port) { scheduler_port_ = sched_port; }

uint16_t PSContext::scheduler_port() const { return scheduler_port_; }

void PSContext::GenerateResetterRound() {
  uint32_t binary_server_context = 0;
  bool is_parameter_server_mode = false;
  bool is_federated_learning_mode = false;
  bool is_mixed_training_mode = false;

  if (server_mode_ == kServerModePS) {
    is_parameter_server_mode = true;
  } else if (server_mode_ == kServerModeFL) {
    is_federated_learning_mode = true;
  } else if (server_mode_ == kServerModeHybrid) {
    is_mixed_training_mode = true;
  } else {
    MS_LOG(EXCEPTION) << server_mode_ << " is invalid. Server mode must be " << kServerModePS << " or " << kServerModeFL
                      << " or " << kServerModeHybrid;
    return;
  }

  binary_server_context = (is_parameter_server_mode << 0) | (is_federated_learning_mode << 1) |
                          (is_mixed_training_mode << 2) | (secure_aggregation_ << 3) | (worker_upload_weights_ << 4);
  if (kServerContextToResetRoundMap.count(binary_server_context) == 0) {
    resetter_round_ = ResetterRound::kNoNeedToReset;
  } else {
    resetter_round_ = kServerContextToResetRoundMap.at(binary_server_context);
  }
  MS_LOG(INFO) << "Server context is " << binary_server_context << ". Resetter round is " << resetter_round_;
  return;
}

ResetterRound PSContext::resetter_round() const { return resetter_round_; }

void PSContext::set_fl_server_port(uint16_t fl_server_port) { fl_server_port_ = fl_server_port; }

uint16_t PSContext::fl_server_port() const { return fl_server_port_; }

void PSContext::set_fl_client_enable(bool enabled) { fl_client_enable_ = enabled; }

bool PSContext::fl_client_enable() { return fl_client_enable_; }

void PSContext::set_start_fl_job_threshold(size_t start_fl_job_threshold) {
  start_fl_job_threshold_ = start_fl_job_threshold;
}

size_t PSContext::start_fl_job_threshold() const { return start_fl_job_threshold_; }

void PSContext::set_fl_name(const std::string &fl_name) { fl_name_ = fl_name; }

const std::string &PSContext::fl_name() const { return fl_name_; }

void PSContext::set_fl_iteration_num(uint64_t fl_iteration_num) { fl_iteration_num_ = fl_iteration_num; }

uint64_t PSContext::fl_iteration_num() const { return fl_iteration_num_; }

void PSContext::set_client_epoch_num(uint64_t client_epoch_num) { client_epoch_num_ = client_epoch_num; }

uint64_t PSContext::client_epoch_num() const { return client_epoch_num_; }

void PSContext::set_client_batch_size(uint64_t client_batch_size) { client_batch_size_ = client_batch_size; }

uint64_t PSContext::client_batch_size() const { return client_batch_size_; }

void PSContext::set_worker_upload_weights(uint64_t worker_upload_weights) {
  worker_upload_weights_ = worker_upload_weights;
}

uint64_t PSContext::worker_upload_weights() const { return worker_upload_weights_; }

void PSContext::set_secure_aggregation(bool secure_aggregation) { secure_aggregation_ = secure_aggregation; }

bool PSContext::secure_aggregation() const { return secure_aggregation_; }
}  // namespace ps
}  // namespace mindspore
