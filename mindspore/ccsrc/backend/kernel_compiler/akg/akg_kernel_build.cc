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

#include "backend/kernel_compiler/akg/akg_kernel_build.h"

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include <iostream>
#include "nlohmann/json.hpp"
#include "ir/dtype.h"
#include "ir/func_graph.h"
#include "utils/context/graph_kernel_flags.h"
#include "backend/kernel_compiler/common_utils.h"
#include "backend/kernel_compiler/akg/akg_kernel_json_generator.h"
#include "backend/kernel_compiler/akg/akg_kernel_attrs_process.h"
#include "backend/session/anf_runtime_algorithm.h"

namespace mindspore {
namespace kernel {

#define INIT_SET_FROM_2D_ARRAY(set_var, list_idx) \
  std::set<size_t> set_var(kernel_lists_[list_idx], kernel_lists_[list_idx] + kernel_lists_[list_idx][kMaxKernelNum_]);

#define LIST_BEGIN(list_idx) kernel_lists_[list_idx]
#define LIST_END(list_idx) (kernel_lists_[list_idx] + kernel_lists_[list_idx][kMaxKernelNum_])
#define RESET_LIST_SIZE(list_idx, val) kernel_lists_[list_idx][kMaxKernelNum_] = val

#define INCREASE_LIST_SIZE(list_idx, val) kernel_lists_[list_idx][kMaxKernelNum_] += val

constexpr int32_t PROCESS_NUM = 16;
constexpr int32_t TIME_OUT = 300;

static inline size_t NameToHashID(const std::string &name) {
  auto idx = name.find_last_of("_");
  auto hash_id_str = name.substr(idx + 1);
  size_t hash_id = 0;
  size_t carry = 1;

  for (int i = static_cast<int>(hash_id_str.size() - 1); i >= 0; i--) {
    hash_id += static_cast<size_t>(std::stoi(hash_id_str.substr(static_cast<size_t>(i), 1))) * carry;
    carry *= 10;
  }

  return hash_id;
}

bool AkgKernelPool::LockMng::TryLock() {
  // Try to lock 100 times. Return errno if lock unsuccessfully
  uint32_t trial = 100;

  int32_t ret = -1;
  while (trial > 0) {
    ret = lockf(fd_, F_TLOCK, 0);
    if (ret == 0 || (errno != EACCES && errno != EAGAIN)) {
      break;
    }

    trial--;
    usleep(5000);
  }

  if (ret == -1) {
    MS_LOG(ERROR) << "Failed to acquire the lock, errno:" << strerror(errno) << ".";
    return false;
  }

  return true;
}

void AkgKernelPool::LockMng::Unlock() {
  auto ret = lockf(fd_, F_ULOCK, 0);
  if (ret == -1) {
    MS_LOG(ERROR) << "Failed to release the lock, errno:" << strerror(errno);
  }
}

std::string AkgKernelPool::GetCurrentPath() {
  char cwd[PATH_MAX];
  char *ret = getcwd(cwd, sizeof(cwd));
  if (ret == nullptr) {
    MS_LOG(ERROR) << "Get current work directory failed, errno:" << strerror(errno);
    return "";
  }

  char abspath[PATH_MAX];
  char *res = realpath(cwd, abspath);
  if (res == nullptr) {
    MS_LOG(ERROR) << "Change to realpath failed, errno:" << strerror(errno);
    return "";
  }

  return std::string(abspath);
}

void *AkgKernelPool::CreateSharedMem(const std::string &path) {
  is_creator_ = false;

  auto hash_id = std::hash<std::string>()(path);
  auto key_id = static_cast<key_t>(hash_id);
  auto mem_size = sizeof(size_t) * kListNum_ * (kMaxKernelNum_ + 1) + 512;

  {
    LockMng lock(fd_);
    if (!lock.locked_) {
      MS_LOG(ERROR) << "Failed to acquire lock.";
      return nullptr;
    }

    // check if the shared memory exists or not.
    // remove shared memory if exists and the nattach is 0
    struct shmid_ds buf;
    auto id = shmget(key_id, mem_size, 0);
    if (id != -1) {
      auto ret = shmctl(id, IPC_STAT, &buf);
      if (ret == -1) {
        MS_LOG(ERROR) << "Failed to get the info of shared memory, errno:" << strerror(errno);
        return nullptr;
      }

      if (buf.shm_nattch == 0) {
        ret = shmctl(id, IPC_RMID, nullptr);
        if (ret < 0) {
          MS_LOG(EXCEPTION) << "Realse shared_mem failed, errno:" << strerror(errno);
        }
      }
    }
  }

  LockMng lock(fd_);
  if (!lock.locked_) {
    MS_LOG(ERROR) << "Failed to acquire lock.";
    return nullptr;
  }

  shm_id_ = shmget(key_id, mem_size, IPC_CREAT | IPC_EXCL | 0600);
  if (shm_id_ == -1) {
    if (errno == EEXIST) {
      shm_id_ = shmget(key_id, mem_size, 0);
    }

    if (shm_id_ == -1) {
      MS_LOG(ERROR) << "Create shared_mem failed, error no:" << strerror(errno);
      return nullptr;
    }
  } else {
    is_creator_ = true;
  }

  auto local_addr = shmat(shm_id_, nullptr, 0);
  if (local_addr == reinterpret_cast<void *>(-1)) {
    MS_LOG(ERROR) << "Attach to shared_mem failed, error no:" << strerror(errno);
    return nullptr;
  }

  if (is_creator_) {
    (void)memset(local_addr, 0, mem_size);
  }

  return local_addr;
}

int32_t AkgKernelPool::Init(const std::vector<JsonNodePair> &build_args) {
  auto cp = GetCurrentPath();
  if (cp.empty()) {
    return -1;
  }

  fd_ = open(kKeyName_, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
  if (fd_ == -1) {
    MS_LOG(ERROR) << "open file <" << kKeyName_ << "> failed, errno:" << strerror(errno);
    return -1;
  }

  auto addr = CreateSharedMem(cp);
  if (addr == nullptr) {
    return -1;
  }

  InitKernelLists(addr);

  auto ret = AddKernels(build_args);
  if (ret != 0) {
    MS_LOG(ERROR) << "AkgKernelPool AddKernels failed.";
    return false;
  }

  return 0;
}

AkgKernelPool::~AkgKernelPool() {
  // Detach shared memory
  auto ret = shmdt(reinterpret_cast<void *>(kernel_lists_[0]));
  if (ret < 0) {
    MS_LOG(EXCEPTION) << "Shared_mem detach failed, errno:" << strerror(errno);
  }

  // Realse shared_memroy
  if (is_creator_) {
    ret = shmctl(shm_id_, IPC_RMID, nullptr);
    if (ret < 0) {
      MS_LOG(EXCEPTION) << "Realse shared_mem failed, errno:" << strerror(errno);
    }
  }

  // Close key file
  if (fd_ != -1) {
    (void)close(fd_);
  }
}

int32_t AkgKernelPool::AddKernels(const std::vector<JsonNodePair> &build_args) {
  LockMng lock(fd_);
  if (!lock.locked_) {
    MS_LOG(ERROR) << "Failed to acquire lock.";
    return -1;
  }

  INIT_SET_FROM_2D_ARRAY(todo_list, kToDoIdx_);
  INIT_SET_FROM_2D_ARRAY(doing_list, kDoingIdx_);
  INIT_SET_FROM_2D_ARRAY(done_list, kDoneIdx_);

  for (const auto &[json_generator, anf_node] : build_args) {
    MS_EXCEPTION_IF_NULL(anf_node);
    auto kernel_name = json_generator.kernel_name();

    auto hash_id = NameToHashID(kernel_name);
    if (self_kernel_ids_.count(hash_id) != 0) {
      MS_LOG(ERROR) << "Duplicated hash_id in list.";
      return -1;
    }

    self_kernel_ids_.emplace(hash_id);
  }

  std::set<size_t> diff_from_todo;
  std::set<size_t> diff_from_doing;
  std::set<size_t> diff_from_done;

  // add the unique kernel only once, so need to check if it exists in todo_list, doing_list, or done_list
  std::set_difference(self_kernel_ids_.begin(), self_kernel_ids_.end(), todo_list.begin(), todo_list.end(),
                      std::inserter(diff_from_todo, diff_from_todo.begin()));
  std::set_difference(diff_from_todo.begin(), diff_from_todo.end(), doing_list.begin(), doing_list.end(),
                      std::inserter(diff_from_doing, diff_from_doing.begin()));
  std::set_difference(diff_from_doing.begin(), diff_from_doing.end(), done_list.begin(), done_list.end(),
                      std::inserter(diff_from_done, diff_from_done.begin()));

  auto new_kernel_size = diff_from_done.size();
  if (new_kernel_size + todo_list.size() > static_cast<size_t>(kMaxKernelNum_)) {
    MS_LOG(ERROR) << "The size of kernels is " << new_kernel_size << ", while the left space of the pool is "
                  << kMaxKernelNum_ - todo_list.size();
    return -1;
  }

  std::copy(diff_from_done.begin(), diff_from_done.end(), LIST_END(kToDoIdx_));
  INCREASE_LIST_SIZE(kToDoIdx_, new_kernel_size);

  return 0;
}

int32_t AkgKernelPool::FetchKernels(std::set<size_t> *out) {
  LockMng lock(fd_);
  if (!lock.locked_) {
    MS_LOG(ERROR) << "Failed to acquire lock.";
    return -1;
  }

  std::set<size_t> left_in_todo_list;

  // filter out kernels which belongs to other processes
  auto FilterBySelfList = [&left_in_todo_list, &out, this](size_t id) {
    if (this->self_kernel_ids_.count(id) != 0) {
      out->emplace(id);
    } else {
      left_in_todo_list.emplace(id);
    }
  };

  std::for_each(LIST_BEGIN(kToDoIdx_), LIST_END(kToDoIdx_), FilterBySelfList);

  std::copy(out->begin(), out->end(), LIST_END(kDoingIdx_));
  INCREASE_LIST_SIZE(kDoingIdx_, out->size());

  std::copy(left_in_todo_list.begin(), left_in_todo_list.end(), LIST_BEGIN(kToDoIdx_));
  RESET_LIST_SIZE(kToDoIdx_, left_in_todo_list.size());

  return 0;
}

int32_t AkgKernelPool::UpdateAndWait(const std::set<size_t> &ids) {
  if (!ids.empty()) {
    LockMng lock(fd_);
    if (!lock.locked_) {
      MS_LOG(ERROR) << "Failed to acquire lock.";
      return -1;
    }

    // update the state of finished kernels to `done`
    std::copy(ids.begin(), ids.end(), LIST_END(kDoneIdx_));
    INCREASE_LIST_SIZE(kDoneIdx_, ids.size());

    // delete the finished kernels from doing_list
    std::vector<size_t> left_in_doing_list;
    INIT_SET_FROM_2D_ARRAY(doing_list, kDoingIdx_);
    std::set_difference(doing_list.begin(), doing_list.end(), ids.begin(), ids.end(),
                        std::inserter(left_in_doing_list, left_in_doing_list.begin()));

    std::copy(left_in_doing_list.begin(), left_in_doing_list.end(), LIST_BEGIN(kDoingIdx_));
    RESET_LIST_SIZE(kDoingIdx_, left_in_doing_list.size());
  }

  auto ret = Wait();
  if (ret != 0) {
    MS_LOG(ERROR) << "AkgKernelPool Wait failed.";
    return -1;
  }

  return 0;
}

int32_t AkgKernelPool::Wait() {
  // wait until all the kernels which belong to this process finish compiling
  uint32_t trials = 1000;

  while (trials > 0) {
    {
      LockMng lock(fd_);
      if (!lock.locked_) {
        MS_LOG(ERROR) << "Failed to acquire lock.";
        return -1;
      }

      INIT_SET_FROM_2D_ARRAY(done_list, kDoneIdx_);

      if (std::all_of(self_kernel_ids_.begin(), self_kernel_ids_.end(),
                      [&done_list](size_t id) { return done_list.count(id) != 0; })) {
        return 0;
      }
    }

    usleep(1000000);
    trials--;
  }

  MS_LOG(ERROR) << "Time out while wait kernel compiling";
  return -1;
}

std::vector<std::string> AkgKernelBuilder::GetNotCachedKernelJsons(const std::vector<JsonNodePair> &build_args) {
  // Remove cached nodes, gether unique nodes, and collect repeated nodes which need postprecess.
  std::vector<std::string> jsons;
  std::unordered_set<std::string> kernel_name_set;
  for (const auto &[json_generator, anf_node] : build_args) {
    MS_EXCEPTION_IF_NULL(anf_node);
    auto kernel_name = json_generator.kernel_name();
    MS_LOG(DEBUG) << "Akg start compile op: " << kernel_name;

    auto cached_kernel_pack = AkgSearchCache(kernel_name);
    if (cached_kernel_pack != nullptr) {
      MS_LOG(DEBUG) << "Use cached kernel, kernel_name[" << kernel_name << "], fullname_with_scope["
                    << anf_node->fullname_with_scope() << "].";
      AkgSetKernelMod(cached_kernel_pack, json_generator, anf_node);
      continue;
    }

    if (kernel_name_set.count(kernel_name) != 0) {
      repeat_nodes_.push_back({json_generator, anf_node});
      continue;
    }
    kernel_name_set.insert(kernel_name);
    auto kernel_json = json_generator.kernel_json_str();
    AkgSaveJsonInfo(kernel_name, kernel_json);
    jsons.push_back(kernel_json);
  }
  return jsons;
}

std::vector<JsonNodePair> AkgKernelBuilder::GetNotCachedKernels(const std::vector<JsonNodePair> &build_args) {
  std::unordered_set<std::string> kernel_name_set;
  std::vector<JsonNodePair> new_build_args;
  for (const auto &[json_generator, anf_node] : build_args) {
    MS_EXCEPTION_IF_NULL(anf_node);
    auto kernel_name = json_generator.kernel_name();

    auto cached_kernel_pack = AkgSearchCache(kernel_name);
    if (cached_kernel_pack != nullptr) {
      MS_LOG(DEBUG) << "Use cached kernel, kernel_name[" << kernel_name << "], fullname_with_scope["
                    << anf_node->fullname_with_scope() << "].";
      AkgSetKernelMod(cached_kernel_pack, json_generator, anf_node);
      continue;
    }

    if (kernel_name_set.count(kernel_name) != 0) {
      repeat_nodes_.push_back({json_generator, anf_node});
      continue;
    }
    kernel_name_set.insert(kernel_name);
    new_build_args.push_back({json_generator, anf_node});
  }
  return new_build_args;
}

bool AkgKernelBuilder::InsertToCache(const std::vector<JsonNodePair> &build_args) {
  for (const auto &[json_generator, anf_node] : build_args) {
    auto kernel_name = json_generator.kernel_name();
    auto new_kernel_pack = AkgInsertCache(kernel_name);
    if (new_kernel_pack == nullptr) {
      MS_LOG(ERROR) << "Insert to cache failed, kernel_name[" << kernel_name << "], fullname_with_scope["
                    << anf_node->fullname_with_scope() << "].";
      return false;
    }
    AkgSetKernelMod(new_kernel_pack, json_generator, anf_node);
    MS_LOG(DEBUG) << "Akg compile " << kernel_name << " kernel and insert cache successfully!";
  }
  return true;
}

bool AkgKernelBuilder::HandleRepeatNodes() {
  for (const auto &[json_generator, anf_node] : repeat_nodes_) {
    auto kernel_name = json_generator.kernel_name();
    auto cached_kernel_pack = AkgSearchCache(kernel_name);
    if (cached_kernel_pack == nullptr) {
      MS_LOG(ERROR) << "Use cached kernel failed, kernel_name[" << kernel_name << "], fullname_with_scope["
                    << anf_node->fullname_with_scope() << "].";
      return false;
    }
    MS_LOG(INFO) << "Use just compiled kernel, kernel_name[" << kernel_name << "], fullname_with_scope["
                 << anf_node->fullname_with_scope() << "].";
    AkgSetKernelMod(cached_kernel_pack, json_generator, anf_node);
  }
  return true;
}

std::vector<std::string> AkgKernelBuilder::GetKernelJsonsByHashId(const std::vector<JsonNodePair> &build_args,
                                                                  std::set<size_t> fetched_ids) {
  std::vector<std::string> jsons;
  for (const auto &[json_generator, anf_node] : build_args) {
    MS_EXCEPTION_IF_NULL(anf_node);
    auto kernel_name = json_generator.kernel_name();

    auto hash_id = NameToHashID(kernel_name);

    if (fetched_ids.count(hash_id) == 0) {
      continue;
    }

    auto kernel_json = json_generator.kernel_json_str();
    AkgSaveJsonInfo(kernel_name, kernel_json);
    jsons.push_back(kernel_json);
  }
  return jsons;
}

bool AkgKernelBuilder::AkgOpParallelBuild(const std::vector<JsonNodePair> &build_args) {
  repeat_nodes_.clear();
  auto new_build_args = GetNotCachedKernels(build_args);
  if (new_build_args.empty()) {
    return true;
  }

  AkgKernelPool kp;
  auto ret = kp.Init(new_build_args);
  if (ret != 0) {
    MS_LOG(ERROR) << "AkgKernelPool init failed.";
    return false;
  }

  std::set<size_t> fetched_ids;
  ret = kp.FetchKernels(&fetched_ids);
  if (ret != 0) {
    MS_LOG(ERROR) << "AkgKernelPool FetchKernels failed.";
    return false;
  }

  if (!fetched_ids.empty()) {
    auto jsons = GetKernelJsonsByHashId(new_build_args, fetched_ids);

    auto client = GetClient();
    MS_EXCEPTION_IF_NULL(client);
    if (!client->AkgStart(PROCESS_NUM, TIME_OUT)) {
      MS_LOG(ERROR) << "Akg start failed.";
      return false;
    }
    auto attrs = CollectBuildAttrs();
    if (!attrs.empty() && !client->AkgSendAttr(attrs)) {
      MS_LOG(ERROR) << "Akg send attr failed.";
      return false;
    }
    if (!client->AkgSendData(jsons)) {
      MS_LOG(ERROR) << "Akg send data failed.";
      return false;
    }
    if (!client->AkgWait()) {
      MS_LOG(ERROR) << "Akg compile failed.";
      return false;
    }
  }

  ret = kp.UpdateAndWait(fetched_ids);
  if (ret != 0) {
    MS_LOG(ERROR) << "AkgKernelPool UpdateAndWait failed.";
    return false;
  }

  // All unique done here, cache them and set kernel.
  if (!InsertToCache(build_args)) {
    MS_LOG(ERROR) << "Insert cache failed.";
    return false;
  }

  if (!HandleRepeatNodes()) {
    MS_LOG(ERROR) << "Handle repeat nodes failed.";
    return false;
  }

  return true;
}

bool AkgKernelBuilder::AkgKernelParallelBuild(const std::vector<AnfNodePtr> &anf_nodes) {
  std::vector<JsonNodePair> json_and_node;
  for (const auto &anf_node : anf_nodes) {
    MS_EXCEPTION_IF_NULL(anf_node);
    DumpOption option;
    option.get_compute_capability = true;
    AkgKernelJsonGenerator akg_kernel_json_generator(option);
    auto cnode = anf_node->cast<CNodePtr>();
    MS_EXCEPTION_IF_NULL(cnode);
    if (AnfAlgo::IsGraphKernel(cnode)) {
      auto func_graph = AnfAlgo::GetCNodeFuncGraphPtr(cnode);
      MS_EXCEPTION_IF_NULL(func_graph);
      auto mng = func_graph->manager();
      if (mng == nullptr) {
        mng = Manage(func_graph, true);
        func_graph->set_manager(mng);
      }
      std::vector<AnfNodePtr> node_list, input_list, output_list;
      GetValidKernelNodes(func_graph, &node_list, &input_list, &output_list);
      if (!akg_kernel_json_generator.CollectFusedJson(node_list, input_list, output_list)) {
        MS_EXCEPTION(UnknownError) << "Collect op info failed. op[" << anf_node->fullname_with_scope() << "].";
      }
    } else {
      if (!akg_kernel_json_generator.CollectJson(anf_node)) {
        MS_EXCEPTION(UnknownError) << "Collect op info failed. op[" << anf_node->fullname_with_scope() << "].";
      }
    }
    json_and_node.push_back({akg_kernel_json_generator, anf_node});
  }

  if (json_and_node.empty()) {
    MS_LOG(DEBUG) << "There is no kernel needed to be compiled.";
    return true;
  }

  struct timeval start_time, end_time;
  (void)gettimeofday(&start_time, nullptr);

  MS_LOG(INFO) << "Akg start parallel build. kernel count: " << json_and_node.size();
  bool res = AkgOpParallelBuild(json_and_node);
  if (!res) {
    MS_LOG(ERROR) << "Akg build kernel failed.";
  }

  (void)gettimeofday(&end_time, nullptr);
  const uint64_t kUSecondInSecond = 1000000;
  uint64_t cost = kUSecondInSecond * static_cast<uint64_t>(end_time.tv_sec - start_time.tv_sec);
  cost += static_cast<uint64_t>(end_time.tv_usec - start_time.tv_usec);
  MS_LOG(INFO) << "Akg kernel build time: " << cost << " us.";
  return true;
}

std::string AkgKernelBuilder::CollectBuildAttrs() {
  auto &flags = context::GraphKernelFlags::GetInstance();
  nlohmann::json attrs;
  if (flags.online_tuning > 0) {
    attrs["online_tuning"] = flags.online_tuning;
  }
  if (!flags.repository_path.empty()) {
    attrs["repository_path"] = flags.repository_path;
  }
  return attrs.empty() ? "" : attrs.dump();
}
}  // namespace kernel
}  // namespace mindspore
