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

#include "thread/actor_threadpool.h"
#include "thread/core_affinity.h"

namespace mindspore {
void ActorWorker::CreateThread(ActorThreadPool *pool) {
  THREAD_RETURN_IF_NULL(pool);
  pool_ = pool;
  thread_ = std::thread(&ActorWorker::RunWithSpin, this);
}

void ActorWorker::RunWithSpin() {
#ifndef __APPLE__
  static std::atomic_int index = {0};
  pthread_setname_np(pthread_self(), ("ActorThread_" + std::to_string(index++)).c_str());
#endif
  while (alive_) {
    // only run either local KernelTask or PoolQueue ActorTask
    if (RunLocalKernelTask() || RunQueueActorTask()) {
      spin_count_ = 0;
    } else {
      YieldAndDeactive();
    }
    if (spin_count_ >= kDefaultSpinCount) {
      WaitUntilActive();
      spin_count_ = 0;
    }
  }
}

bool ActorWorker::RunQueueActorTask() {
  THREAD_ERROR_IF_NULL(pool_);
  auto actor = pool_->PopActorFromQueue();
  if (actor == nullptr) {
    return false;
  }
  actor->Run();
  return true;
}

bool ActorWorker::Active() {
  {
    std::lock_guard<std::mutex> _l(mutex_);
    if (status_ != kThreadIdle) {
      return false;
    }
    status_ = kThreadBusy;
  }
  cond_var_.notify_one();
  return true;
}

ActorThreadPool::~ActorThreadPool() {
  // wait until actor queue is empty
  bool terminate = false;
  do {
    {
      std::lock_guard<std::mutex> _l(actor_mutex_);
      terminate = actor_queue_.empty();
    }
    if (!terminate) {
      std::this_thread::yield();
    }
  } while (!terminate);
  for (auto &worker : workers_) {
    delete worker;
    worker = nullptr;
  }
  workers_.clear();
}

ActorReference ActorThreadPool::PopActorFromQueue() {
  std::lock_guard<std::mutex> _l(actor_mutex_);
  if (actor_queue_.empty()) {
    return nullptr;
  }
  auto actor = actor_queue_.front();
  actor_queue_.pop();
  return actor;
}

void ActorThreadPool::PushActorToQueue(const ActorReference &actor) {
  {
    std::lock_guard<std::mutex> _l(actor_mutex_);
    actor_queue_.push(actor);
  }
  THREAD_INFO("actor[%s] enqueue success", actor->GetAID().Name().c_str());
  // active one idle actor thread if exist
  for (size_t i = 0; i < actor_thread_num_; ++i) {
    auto worker = reinterpret_cast<ActorWorker *>(workers_[i]);
    if (worker->Active()) {
      break;
    }
  }
}

int ActorThreadPool::CreateThreads(size_t actor_thread_num, size_t all_thread_num) {
  size_t core_num = std::thread::hardware_concurrency();
  THREAD_INFO("ThreadInfo, Actor: [%zu], All: [%zu], CoreNum: [%zu]", actor_thread_num, all_thread_num, core_num);
  actor_thread_num_ = actor_thread_num < core_num ? actor_thread_num : core_num;
  if (actor_thread_num_ <= 0 || actor_thread_num > all_thread_num) {
    THREAD_ERROR("thread num is invalid");
    return THREAD_ERROR;
  }
  for (size_t i = 0; i < actor_thread_num_; ++i) {
    std::lock_guard<std::mutex> _l(pool_mutex_);
    auto worker = new (std::nothrow) ActorWorker();
    THREAD_ERROR_IF_NULL(worker);
    worker->CreateThread(this);
    workers_.push_back(worker);
    THREAD_INFO("create actor thread[%zu]", i);
  }
  size_t kernel_thread_num = all_thread_num - actor_thread_num_;
  if (kernel_thread_num > 0) {
    return ThreadPool::CreateThreads(kernel_thread_num);
  }
  return THREAD_OK;
}

ActorThreadPool *ActorThreadPool::CreateThreadPool(size_t actor_thread_num, size_t all_thread_num) {
  ActorThreadPool *pool = new (std::nothrow) ActorThreadPool();
  if (pool == nullptr) {
    return nullptr;
  }
  int ret = pool->CreateThreads(actor_thread_num, all_thread_num);
  if (ret != THREAD_OK) {
    delete pool;
    return nullptr;
  }
#ifdef BIND_CORE
  ret = pool->InitAffinityInfo();
  if (ret != THREAD_OK) {
    delete pool;
    return nullptr;
  }
#endif  // BIND_CORE
  return pool;
}

ActorThreadPool *ActorThreadPool::CreateThreadPool(size_t thread_num) {
  ActorThreadPool *pool = new (std::nothrow) ActorThreadPool();
  if (pool == nullptr) {
    return nullptr;
  }
  int ret = pool->CreateThreads(thread_num, thread_num);
  if (ret != THREAD_OK) {
    delete pool;
    return nullptr;
  }
  return pool;
}
}  // namespace mindspore
