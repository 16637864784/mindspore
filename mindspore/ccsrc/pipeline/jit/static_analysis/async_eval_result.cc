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

#include "pipeline/jit/static_analysis/async_eval_result.h"
#include <debug/trace.h>
#include "utils/symbolic.h"
#include "debug/common.h"
#include "pipeline/jit/base.h"
#include "utils/utils.h"

namespace mindspore {
namespace abstract {
HealthPointMgr HealthPointMgr::instance_;

void HealthPointMgr::Clear() {
  MS_LOG(DEBUG) << " Point: " << point_;
  point_ = 1;
}

void HealthPointMgr::HandleException() {
  // Just record the first exception information.
  if (!StaticAnalysisException::Instance().HasException()) {
    StaticAnalysisException::Instance().SetException();
    try {
      // We want to call the LogWrite::^() here.
      MS_LOG(EXCEPTION) << "Exception happened, check the information as below.";
    } catch (const std::exception &e) {
      // Ignored.
    }
  }
  // Free all the locks. Let all the threads continue to run.
  std::lock_guard<std::recursive_mutex> lock(lock_);
  for (auto &item : asyncAbstractList_) {
    item->SetRunnable();
  }
  asyncAbstractList_.clear();
}

void HealthPointMgr::SetNextRunnable() {
  std::lock_guard<std::recursive_mutex> lock(lock_);
  if (asyncAbstractList_.empty()) {
    MS_LOG(DEBUG) << "The Health List is empty. ";
    return;
  }
  // Check if enter endless loop
  auto it = std::find_if(asyncAbstractList_.begin(), asyncAbstractList_.end(), [](const auto &item) {
    MS_EXCEPTION_IF_NULL(item);
    return item->HasResult();
  });
  if (it == asyncAbstractList_.end()) {
    // Enter endless loop if there is not ready result.
    MS_LOG(EXCEPTION) << "Enter endless loop. There is not more node that can been evaluated. Please check the code.";
  }
  // Push back the not ready async.
  asyncAbstractList_.insert(asyncAbstractList_.end(), asyncAbstractList_.begin(), it);
  asyncAbstractList_.erase(asyncAbstractList_.begin(), it);
  MS_EXCEPTION_IF_NULL(asyncAbstractList_.front());
  MS_LOG(DEBUG) << " The Health Point is " << point_ << " Called times : " << asyncAbstractList_.front()->count();
  asyncAbstractList_.front()->SetRunnable();
  asyncAbstractList_.pop_front();
}

AnalysisResultCacheMgr AnalysisResultCacheMgr::instance_;

void AnalysisResultCacheMgr::Clear() {
  std::lock_guard<std::mutex> lock(lock_);
  cache_.clear();
  switch_cache_.clear();
  todo_.clear();
  waiting_.clear();
}

// The thread id format is XXXX.YYYY.ZZZZ
thread_local static std::string local_threadid;
void AnalysisResultCacheMgr::UpdateCaller(const std::string &caller) {
  std::ostringstream buffer;
  buffer << caller << "." << std::this_thread::get_id();
  local_threadid = buffer.str();
}

std::string &AnalysisResultCacheMgr::GetThreadid() { return local_threadid; }

void AnalysisResultCacheMgr::PushToWait(std::future<void> &&future) {
  std::lock_guard<std::mutex> lock(lock_);
  waiting_.emplace_back(std::move(future));
}

void AnalysisResultCacheMgr::PushTodo(const AnfNodeConfigPtr &conf) {
  std::lock_guard<std::mutex> lock(todo_lock_);
  todo_.push_back(conf);
}

void AnalysisResultCacheMgr::InitSwitchValue(const AnfNodeConfigPtr &conf) {
  std::lock_guard<std::mutex> lock(lock_);
  AsyncAbstractPtr async_eval_result = switch_cache_.get(conf);
  if (async_eval_result == nullptr) {
    async_eval_result = std::make_shared<AsyncAbstract>();
    switch_cache_.set(conf, async_eval_result);
  }
}

AbstractBasePtr AnalysisResultCacheMgr::TryGetSwitchValue(const AnfNodeConfigPtr &conf) {
  // don't call lock_.lock(). switch_cache is protected. and it waits for result.
  AsyncAbstractPtr async_eval_result = switch_cache_.get(conf);
  // Conf has been visited and set value.
  if (async_eval_result != nullptr) {
    return async_eval_result->TryGetResult();
  }
  return nullptr;
}

AbstractBasePtr AnalysisResultCacheMgr::GetSwitchValue(const AnfNodeConfigPtr &conf) {
  StaticAnalysisException::Instance().CheckException();
  // don't call lock_.lock(). switch_cache is protected. and it waits for result.
  AsyncAbstractPtr async_eval_result = switch_cache_.get(conf);
  // Conf has been visited and set value.
  if (async_eval_result != nullptr) {
    // Add to schedule
    HealthPointMgr::GetInstance().Add2Schedule(async_eval_result);
    // Maybe blocked for waiting. AsyncAbstract maybe null, if time out.
    auto result = async_eval_result->GetResult();
    if (result == nullptr) {
      result = std::make_shared<AbstractTimeOut>();
      MS_LOG(ERROR) << "AsyncAbstract of NodeConfig " << conf->node()->ToString()
                    << " is nullptr. There is something wrong.";
      StaticAnalysisException::Instance().CheckException();
    }
    return result;
  }
  return nullptr;
}

void AnalysisResultCacheMgr::SetSwitchValue(const AnfNodeConfigPtr &conf, const AbstractBasePtr &arg) {
  MS_EXCEPTION_IF_NULL(conf);
  if (arg == nullptr) {
    MS_LOG(EXCEPTION) << conf->ToString() << " value is nullptr.";
  }
  std::lock_guard<std::mutex> lock(lock_);
  AsyncAbstractPtr async_eval_result = switch_cache_.get(conf);
  if (async_eval_result == nullptr) {
    async_eval_result = std::make_shared<AsyncAbstract>();
    async_eval_result->SetResult(arg);
    switch_cache_.set(conf, async_eval_result);
  } else {
    auto ab1 = async_eval_result->TryGetResult();
    AbstractBasePtrList absList;
    if (ab1 != nullptr) {
      absList.push_back(arg);
      absList.push_back(ab1);
      // Join two branches's result
      auto joined_result = AnalysisEngine::ProcessEvalResults(absList, conf->node());
      async_eval_result->SetResult(joined_result->abstract());
      if (!(*joined_result == *ab1)) {
        PushTodo(conf);
      }
    } else {
      async_eval_result->SetResult(arg);
    }
  }
}

void AnalysisResultCacheMgr::Todo() {
  std::lock_guard<std::mutex> lock(todo_lock_);
  while (!todo_.empty()) {
    AnfNodeConfigPtr conf = todo_.front();
    MS_EXCEPTION_IF_NULL(conf);
    todo_.pop_front();
    if (GetValue(conf) == nullptr) {
      MS_LOG(WARNING) << (conf->node() ? conf->node()->ToString() : "null node") << " not in globle cache.";
      continue;
    }
    if (TryGetSwitchValue(conf) == nullptr) {
      MS_LOG(WARNING) << (conf->node() ? conf->node()->ToString() : "null node") << " not in switch cache.";
      continue;
    }
    auto switch_value = TryGetSwitchValue(conf);
    auto abstract = GetValue(conf)->abstract();
    MS_EXCEPTION_IF_NULL(switch_value);
    MS_EXCEPTION_IF_NULL(abstract);
    if (!(*abstract == *switch_value)) {
      MS_LOG(WARNING) << " Switch Value is not eq. "
                      << " switchCache: " << switch_value->ToString() << " globleCache: " << abstract->ToString()
                      << "\t\tConf: " << conf->ToString();
    }
  }
}

void AnalysisResultCacheMgr::Wait() {
  py::gil_scoped_release infer_gil_release;
  // Check all the async to finish.
  HealthPointScopedDrop hpCheck;
  while (true) {
    lock_.lock();
    if (waiting_.empty()) {
      lock_.unlock();
      break;
    }
    auto future = std::move(waiting_.front());
    waiting_.pop_front();
    lock_.unlock();

    // Must be unlock
    future.wait();
  }
  if (IS_OUTPUT_ON(DEBUG)) {
    Todo();
  }
  MS_LOG(INFO) << "Infer finished.";
}

std::string ArgsToString(const AbstractBasePtrList &args_spec_list) {
  std::ostringstream buffer;
  buffer << "(";
  for (const auto &item : args_spec_list) {
    buffer << item->ToString() << " # ";
  }
  buffer << " )";
  return buffer.str();
}
}  // namespace abstract
}  // namespace mindspore
