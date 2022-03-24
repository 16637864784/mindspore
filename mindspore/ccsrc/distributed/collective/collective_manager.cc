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

#include "distributed/collective/collective_manager.h"
#include <string>
#include <vector>
#include <memory>
#include "utils/ms_context.h"
#include "runtime/recovery/recovery_context.h"

namespace mindspore {
namespace distributed {
namespace collective {
CollectiveManager::CollectiveManager()
    : inited_(false),
      finalized_(true),
      host_ctx_(nullptr),
      device_ctx_(nullptr),
      host_comm_lib_instance_(nullptr),
      device_comm_lib_instance_(nullptr),
      global_rank_id_(0),
      local_rank_id_(0),
      global_rank_size_(0),
      global_group_ranks_({}) {}

CollectiveManager::~CollectiveManager() {
  if (!finalized_) {
    try {
      (void)Finalize();
    } catch (std::exception &) {
      MS_LOG(ERROR) << "Failed to finalize collective manager.";
    }
  }
  finalized_ = true;
  host_ctx_ = nullptr;
  device_ctx_ = nullptr;
  host_comm_lib_instance_ = nullptr;
  device_comm_lib_instance_ = nullptr;
}

std::shared_ptr<CollectiveManager> CollectiveManager::instance() {
  static std::shared_ptr<CollectiveManager> instance = nullptr;
  if (instance == nullptr) {
    instance.reset(new (std::nothrow) CollectiveManager());
    MS_EXCEPTION_IF_NULL(instance);
  }
  return instance;
}

bool CollectiveManager::Initialize() {
  if (inited_ && !runtime::recovery::RecoveryContext::GetInstance()->need_reinit_collective()) {
    return true;
  }

  device_type_ = MsContext::GetInstance()->get_param<std::string>(MS_CTX_DEVICE_TARGET);
  MS_LOG(INFO) << "Start initializing collective communication for backend: " << device_type_ << "...";

  // Step 1: Initialize host side collective communication.
  if (!InitHostCommlib()) {
    MS_LOG(ERROR) << "Failed to initialize host communication library.";
    return false;
  }

  // Step 2, 3 and 4 are for device communication library. So if the training job is only launched on CPU, they will not
  // be necessary.
  // Step 2: Assign local rank id(device id) for this process.
  if (!AssignLocalRank()) {
    MS_LOG(ERROR) << "Failed to assign local rank id.";
    return false;
  }

  // Step 3: Initialize device side collective communication.
  if (!InitDeviceCommLib()) {
    MS_LOG(ERROR) << "Failed to initialize device communication library.";
    return false;
  }

  // Step 4: Create global communication group.
  MS_EXCEPTION_IF_NULL(device_comm_lib_instance_);
  if (!CreateCommunicationGroup(device_comm_lib_instance_->global_group_name(), global_group_ranks_)) {
    MS_LOG(ERROR) << "Failed to initialize host communication library.";
    return false;
  }

  MS_LOG(INFO) << "End initializing collective communication for backend: " << device_type_;
  inited_ = true;
  finalized_ = false;
  return true;
}

bool CollectiveManager::CreateCommunicationGroup(const std::string &group_name,
                                                 const std::vector<uint32_t> &group_ranks) {
  MS_EXCEPTION_IF_NULL(host_comm_lib_instance_);
  MS_EXCEPTION_IF_NULL(device_comm_lib_instance_);
  // Step 1: Create communication group on host side if.
  if (!host_comm_lib_instance_->CreateCommunicationGroup(group_name, group_ranks)) {
    MS_LOG(ERROR) << "Failed to create communication group " << group_name << " on host side.";
    return false;
  }

  // Step 2: Create communication group on device side.
  if (!device_comm_lib_instance_->CreateCommunicationGroup(group_name, group_ranks)) {
    MS_LOG(ERROR) << "Failed to create communication group " << group_name << " on device side.";
    return false;
  }

  // Step 3: Generate device information of the root node.
  CommunicationGroupPtr group = device_comm_lib_instance_->GetGroup(group_name);
  MS_EXCEPTION_IF_NULL(group);
  bool is_root_node = (group->GetGroupRank(global_rank_id_) == 0);
  size_t root_info_size = 0;
  void *root_info = group->GenerateRootInfo(&root_info_size);
  MS_EXCEPTION_IF_NULL(root_info);

  // Step 4: Broadcast the device root information to all nodes on host side.
  if (!host_comm_lib_instance_->BroadcastUniqueID(group_name, is_root_node, root_info_size, root_info)) {
    MS_LOG(ERROR) << "Broadcast for device root info failed on the host side.";
    return false;
  }

  // Step 5: Initialize communication group on the device side.
  return InitDeviceCommGroup(group, root_info);
}

bool CollectiveManager::InitDeviceCommGroup(const CommunicationGroupPtr &group, void *root_info) {
  bool init_group_success = false;
  bool init_group_fail = false;
  std::condition_variable thread_blocker;
  init_group_thread_ = std::make_unique<std::thread>([&, this] {
    device_ctx_->Initialize();
    if (!group->Initialize(root_info)) {
      MS_LOG(ERROR) << "Initialize group on the device side failed.";
      std::unique_lock<std::mutex> lock(init_group_mutex_);
      init_group_fail = true;
      thread_blocker.notify_one();
      return;
    }

    {
      std::unique_lock<std::mutex> lock(init_group_mutex_);
      init_group_success = true;
      thread_blocker.notify_one();
    }
  });
  init_group_thread_->detach();

  // Timeout limit 180 seconds to wait finishing init device communication group.
  const int64_t kTimeToWait = 180;
  std::unique_lock<std::mutex> locker(init_group_mutex_);
  (void)thread_blocker.wait_for(locker, std::chrono::seconds(kTimeToWait),
                                [&] { return init_group_success || init_group_fail; });

  if (!init_group_success && runtime::recovery::RecoveryContext::GetInstance()->enable_recovery()) {
    runtime::recovery::RecoveryContext::GetInstance()->set_recovery_status(
      runtime::recovery::RecoveryErrCode::kInitNcclFailed);
    MS_LOG(ERROR) << "Initialize group on the device side failed.";
  }
  return init_group_success;
}

bool CollectiveManager::DestroyCommunicationGroup(const std::string &group_name) {
  MS_EXCEPTION_IF_NULL(host_comm_lib_instance_);
  if (!host_comm_lib_instance_->DestroyCommunicationGroup(group_name)) {
    MS_LOG(ERROR) << "Failed to destroy communication group of " << group_name << " on the host side.";
    return false;
  }

  MS_EXCEPTION_IF_NULL(device_comm_lib_instance_);
  if (!device_comm_lib_instance_->DestroyCommunicationGroup(group_name)) {
    MS_LOG(ERROR) << "Failed to destroy communication group of " << group_name << " on the device side.";
    return false;
  }
  return true;
}

uint32_t CollectiveManager::GetRankId(const std::string &group_name) {
  MS_EXCEPTION_IF_NULL(host_comm_lib_instance_);
  return host_comm_lib_instance_->GetRankId(group_name);
}

uint32_t CollectiveManager::GetGroupSize(const std::string &group_name) {
  MS_EXCEPTION_IF_NULL(host_comm_lib_instance_);
  return host_comm_lib_instance_->GetGroupSize(group_name);
}

bool CollectiveManager::Finalize() {
  if (finalized_) {
    return true;
  }

  MS_EXCEPTION_IF_NULL(host_comm_lib_instance_);
  if (!host_comm_lib_instance_->Finalize()) {
    MS_LOG(WARNING) << "Failed to finalize host communication library.";
  }

  MS_EXCEPTION_IF_NULL(device_comm_lib_instance_);
  if (!device_comm_lib_instance_->Finalize()) {
    MS_LOG(WARNING) << "Failed to finalize device communication library.";
  }

  finalized_ = true;
  return true;
}

void CollectiveManager::set_global_rank_id(uint32_t global_rank_id) { global_rank_id_ = global_rank_id; }

void CollectiveManager::set_global_rank_size(uint32_t global_rank_size) { global_rank_size_ = global_rank_size; }

uint32_t CollectiveManager::local_rank_id() const { return local_rank_id_; }

bool CollectiveManager::InitHostCommlib() {
  device::DeviceContextKey host_key = {"CPU", 0};
  host_ctx_ = device::DeviceContextManager::GetInstance().GetOrCreateDeviceContext(host_key);
  MS_EXCEPTION_IF_NULL(host_ctx_);
  if (!host_ctx_->LoadCollectiveCommLib()) {
    MS_LOG(ERROR) << "Failed to load communication library on the host side.";
    return false;
  }
  host_comm_lib_instance_ = host_ctx_->collective_comm_lib();
  MS_EXCEPTION_IF_NULL(host_comm_lib_instance_);

  // For some communication libraries, global_rank_id_', 'global_rank_size_' should be set by caller, e.g., when using
  // MindSpore communication. For other communication libraries, global rank id and size is generated by itself, e.g.,
  // OpenMPI, and parameters 'global_rank_id_', 'global_rank_size_' will not be used.
  MS_LOG(INFO) << "Start initializing communication library on host side...";
  if (!host_comm_lib_instance_->Initialize(global_rank_id_, global_rank_size_)) {
    MS_LOG(ERROR) << "Failed to initialize communication library on host side.";
    return false;
  }

  if (!global_group_ranks_.empty()) {
    global_group_ranks_.clear();
  }

  // Reassign 'global_rank_id_' and 'global_rank_size_'. Generate global communication group ranks.
  global_rank_id_ = host_comm_lib_instance_->global_rank_id();
  global_rank_size_ = host_comm_lib_instance_->global_rank_size();
  for (uint32_t i = 0; i < global_rank_size_; i++) {
    global_group_ranks_.push_back(i);
  }

  // Create world group on host side for AllGather operation of host name while assigning local rank.
  host_global_group_name_ = host_comm_lib_instance_->global_group_name();
  if (!host_comm_lib_instance_->CreateCommunicationGroup(host_global_group_name_, global_group_ranks_)) {
    MS_LOG(ERROR) << "Failed to create communication group " << host_global_group_name_ << " on host side.";
    return false;
  }

  MS_LOG(INFO) << "Communication library on host side is successfully initialized. Global rank id: " << global_rank_id_
               << ", global rank size: " << global_rank_size_;
  return true;
}

bool CollectiveManager::InitDeviceCommLib() {
  device::DeviceContextKey device_key = {device_type_, local_rank_id_};
  device_ctx_ = device::DeviceContextManager::GetInstance().GetOrCreateDeviceContext(device_key);
  MS_EXCEPTION_IF_NULL(device_ctx_);
  // We can initialize device context now because device id(local_rank_id_) is already assigned.
  device_ctx_->Initialize();

  if (!device_ctx_->LoadCollectiveCommLib()) {
    MS_LOG(ERROR) << "Failed to load communication library on the device side.";
    return false;
  }
  device_comm_lib_instance_ = device_ctx_->collective_comm_lib();
  MS_EXCEPTION_IF_NULL(device_comm_lib_instance_);

  MS_LOG(INFO) << "Start initializing communication library on device side...";
  if (!device_comm_lib_instance_->Initialize(global_rank_id_, global_rank_size_)) {
    MS_LOG(ERROR) << "Failed to initialize communication library on device side.";
    return false;
  }
  MS_LOG(INFO) << "Communication library on device side is successfully initialized.";
  return true;
}

bool CollectiveManager::AssignLocalRank() {
  char host_name[MAX_HOSTNAME_LEN] = {0};
#ifndef _WIN32
  if (gethostname(host_name, MAX_HOSTNAME_LEN) != 0) {
    MS_LOG(ERROR) << "Failed to get host name.";
    return false;
  }
#endif
  MS_LOG(INFO) << "Host name for rank " << global_rank_id_ << " is " << host_name;

  // Generate host name hash for every process. The host names of different physical machine should not be the same so
  // that local rank id won't repeat.
  size_t host_hash = std::hash<std::string>()(host_name);
  const uint32_t kGlobalRankSize = global_rank_size_;
  std::vector<size_t> all_host_hashs(kGlobalRankSize);
  if (global_rank_id_ >= global_rank_size_) {
    MS_LOG(ERROR) << "The global rank id " << global_rank_id_ << " should be less than global rank size "
                  << global_rank_size_;
    return false;
  }
  all_host_hashs[global_rank_id_] = host_hash;

  MS_EXCEPTION_IF_NULL(host_comm_lib_instance_);
  if (!host_comm_lib_instance_->AllGatherHostHashName(host_hash, &all_host_hashs)) {
    MS_LOG(ERROR) << "AllGather for host names failed.";
    return false;
  }

  // Accumulate rank id.
  // In disaster recovery scenario, this function will enter multiple times when the network is reconfigured, so old
  // local rank id need to be cleaned.
  local_rank_id_ = 0;
  for (uint32_t rank = 0; rank < global_rank_size_; rank++) {
    if (rank == global_rank_id_) {
      break;
    }
    if (all_host_hashs[rank] == all_host_hashs[global_rank_id_]) {
      local_rank_id_++;
    }
  }

  MsContext::GetInstance()->set_param<uint32_t>(MS_CTX_DEVICE_ID, local_rank_id_);
  MS_LOG(INFO) << "The local rank id assigned for this process is " << local_rank_id_
               << ". device_id of ms_context is set.";
  return true;
}
}  // namespace collective
}  // namespace distributed
}  // namespace mindspore
