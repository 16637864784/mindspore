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

#include "tools/time_profiler/time_profiler.h"
#define __STDC_FORMAT_MACROS
#include <cinttypes>
#undef __STDC_FORMAT_MACROS
#include <cmath>
#include <algorithm>
#include <utility>
#include "include/ms_tensor.h"
#include "utils/log_adapter.h"
#include "include/context.h"

namespace mindspore {
namespace lite {
int TimeProfiler::GenerateRandomData(size_t size, void *data) {
  MS_ASSERT(data != nullptr);
  char *castedData = static_cast<char *>(data);
  for (size_t i = 0; i < size; i++) {
    castedData[i] = static_cast<char>(i);
  }
  return RET_OK;
}

int TimeProfiler::GenerateInputData() {
  for (auto tensor : ms_inputs_) {
    MS_ASSERT(tensor != nullptr);
    auto input_data = tensor->MutableData();
    if (input_data == nullptr) {
      MS_LOG(ERROR) << "MallocData for inTensor failed";
      std::cerr << "MallocData for inTensor failed" << std::endl;
      return RET_ERROR;
    }
    MS_ASSERT(tensor->GetData() != nullptr);
    auto tensor_byte_size = tensor->Size();
    auto status = GenerateRandomData(tensor_byte_size, input_data);
    if (status != RET_OK) {
      MS_LOG(ERROR) << "Generate RandomData for inTensor failed " << status;
      std::cerr << "Generate RandomData for inTensor failed " << status << std::endl;
      return RET_ERROR;
    }
  }
  return RET_OK;
}

int TimeProfiler::ReadInputFile() {
  if (ms_inputs_.empty()) {
    return RET_OK;
  }

  auto inTensor = ms_inputs_.at(0);
  MS_ASSERT(inTensor != nullptr);

  size_t size;
  char *bin_buf = ReadFile(_flags->in_data_path_.c_str(), &size);
  if (bin_buf == nullptr) {
    MS_LOG(ERROR) << "Read input data failed.";
    std::cerr << "Read input data failed." << std::endl;
    return RET_ERROR;
  }
  auto tensor_data_size = inTensor->Size();
  if (size != tensor_data_size) {
    MS_LOG(ERROR) << "Input binary file size error, required: " << tensor_data_size << " in fact: " << size;
    std::cerr << "Input binary file size error, required: " << tensor_data_size << " in fact: " << size << std::endl;
    return RET_ERROR;
  }
  auto input_data = inTensor->MutableData();
  memcpy(input_data, bin_buf, tensor_data_size);
  return RET_OK;
}

int TimeProfiler::LoadInput() {
  ms_inputs_ = session_->GetInputs();
  if (_flags->in_data_path_.empty()) {
    auto status = GenerateInputData();
    if (status != RET_OK) {
      MS_LOG(ERROR) << "Generate input data error " << status;
      std::cerr << "Generate input data error " << status << std::endl;
      return RET_ERROR;
    }
  } else {
    auto status = ReadInputFile();
    if (status != RET_OK) {
      MS_LOG(ERROR) << "ReadInputFile error " << status;
      std::cerr << "ReadInputFile error " << status << std::endl;
      return RET_ERROR;
    }
  }
  return RET_OK;
}

int TimeProfiler::InitSession() {
  size_t size = 0;
  char *graph_buf = ReadFile(_flags->model_path_.c_str(), &size);
  if (graph_buf == nullptr) {
    MS_LOG(ERROR) << "Load graph failed, path " << _flags->model_path_;
    std::cerr << "Load graph failed, path " << _flags->model_path_ << std::endl;
    return RET_ERROR;
  }

  auto ctx = new lite::Context;
  ctx->cpu_bind_mode_ = static_cast<CpuBindMode>(_flags->cpu_bind_mode_);
  ctx->device_type_ = lite::DT_CPU;
  ctx->thread_num_ = _flags->num_threads_;
  ctx->float16_priority = _flags->fp16_priority;
  session_ = session::LiteSession::CreateSession(ctx);
  if (session_ == nullptr) {
    MS_LOG(ERROR) << "New session failed while running.";
    std::cerr << "New session failed while running." << std::endl;
    return RET_ERROR;
  }

  return RET_OK;
}

int TimeProfiler::InitCallbackParameter() {
  // before callback
  before_call_back_ = [&](const std::vector<mindspore::tensor::MSTensor *> &before_inputs,
                          const std::vector<mindspore::tensor::MSTensor *> &before_outputs,
                          const session::CallBackParam &callParam) {
    if (before_inputs.empty()) {
      MS_LOG(INFO) << "The num of beforeInputs is empty";
    }
    if (before_outputs.empty()) {
      MS_LOG(INFO) << "The num of beforeOutputs is empty";
    }
    if (op_times_by_type_.find(callParam.type_callback_param) == op_times_by_type_.end()) {
      op_times_by_type_.insert(std::make_pair(callParam.type_callback_param, std::make_pair(0, 0.0f)));
    }
    if (op_times_by_name_.find(callParam.name_callback_param) == op_times_by_name_.end()) {
      op_times_by_name_.insert(std::make_pair(callParam.name_callback_param, std::make_pair(0, 0.0f)));
    }

    op_call_times_total_++;
    op_begin_ = GetTimeUs();
    return true;
  };

  // after callback
  after_call_back_ = [&](const std::vector<mindspore::tensor::MSTensor *> &after_inputs,
                         const std::vector<mindspore::tensor::MSTensor *> &after_outputs,
                         const session::CallBackParam &call_param) {
    uint64_t opEnd = GetTimeUs();

    if (after_inputs.empty()) {
      MS_LOG(INFO) << "The num of after inputs is empty";
    }
    if (after_outputs.empty()) {
      MS_LOG(INFO) << "The num of after outputs is empty";
    }

    float cost = static_cast<float>(opEnd - op_begin_) / 1000.0f;
    op_cost_total_ += cost;
    op_times_by_type_[call_param.type_callback_param].first++;
    op_times_by_type_[call_param.type_callback_param].second += cost;
    op_times_by_name_[call_param.name_callback_param].first++;
    op_times_by_name_[call_param.name_callback_param].second += cost;
    return true;
  };

  return RET_OK;
}

int TimeProfiler::Init() {
  if (this->_flags == nullptr) {
    return 1;
  }
  MS_LOG(INFO) << "ModelPath = " << _flags->model_path_;
  MS_LOG(INFO) << "InDataPath = " << _flags->in_data_path_;
  MS_LOG(INFO) << "LoopCount = " << _flags->loop_count_;
  MS_LOG(INFO) << "NumThreads = " << _flags->num_threads_;
  MS_LOG(INFO) << "Fp16Priority = " << _flags->fp16_priority;

  if (_flags->num_threads_ < 1) {
    MS_LOG(ERROR) << "NumThreads: " << _flags->num_threads_ << " must greater than or equal 1";
    std::cerr << "NumThreads: " << _flags->num_threads_ << " must greater than or equal 1" << std::endl;
    return RET_ERROR;
  }

  if (_flags->loop_count_ < 1) {
    MS_LOG(ERROR) << "LoopCount: " << _flags->loop_count_ << " must greater than or equal 1";
    std::cerr << "LoopCount: " << _flags->loop_count_ << " must greater than or equal 1" << std::endl;
    return RET_ERROR;
  }

  if (_flags->cpu_bind_mode_ == CpuBindMode::MID_CPU) {
    MS_LOG(INFO) << "cpuBindMode = MID_CPU";
  } else if (_flags->cpu_bind_mode_ == CpuBindMode::HIGHER_CPU) {
    MS_LOG(INFO) << "cpuBindMode = HIGHER_CPU";
  } else if (_flags->cpu_bind_mode_ == CpuBindMode::NO_BIND) {
    MS_LOG(INFO) << "cpuBindMode = NO_BIND";
  } else {
    MS_LOG(ERROR) << "cpuBindMode Error";
    return RET_ERROR;
  }

  if (_flags->model_path_.empty()) {
    MS_LOG(ERROR) << "modelPath is required";
    std::cerr << "modelPath is required" << std::endl;
    return RET_ERROR;
  }

  auto status = InitSession();
  if (status != RET_OK) {
    MS_LOG(ERROR) << "Init session failed.";
    std::cerr << "Init session failed." << std::endl;
    return RET_ERROR;
  }

  status = this->LoadInput();
  if (status != RET_OK) {
    MS_LOG(ERROR) << "Load input failed.";
    std::cerr << "Load input failed." << std::endl;
    return RET_ERROR;
  }

  status = InitCallbackParameter();
  if (status != RET_OK) {
    MS_LOG(ERROR) << "Init callback Parameter failed.";
    std::cerr << "Init callback Parameter failed." << std::endl;
    return RET_ERROR;
  }

  return RET_OK;
}

int TimeProfiler::PrintResult(const std::vector<std::string> &title,
                             const std::map<std::string, std::pair<int, float>> &result) {
  std::vector<size_t> columnLenMax(5);
  std::vector<std::vector<std::string>> rows;

  for (auto &iter : result) {
    char stringBuf[5][100] = {};
    std::vector<std::string> columns;
    size_t len;

    len = iter.first.size();
    if (len > columnLenMax.at(0)) {
      columnLenMax.at(0) = len + 4;
    }
    columns.push_back(iter.first);

    len = snprintf(stringBuf[1], sizeof(stringBuf[1]), "%f", iter.second.second / _flags->loop_count_);
    if (len > columnLenMax.at(1)) {
      columnLenMax.at(1) = len + 4;
    }
    columns.emplace_back(stringBuf[1]);

    len = snprintf(stringBuf[2], sizeof(stringBuf[2]), "%f", iter.second.second / op_cost_total_);
    if (len > columnLenMax.at(2)) {
      columnLenMax.at(2) = len + 4;
    }
    columns.emplace_back(stringBuf[2]);

    len = snprintf(stringBuf[3], sizeof(stringBuf[3]), "%d", iter.second.first);
    if (len > columnLenMax.at(3)) {
      columnLenMax.at(3) = len + 4;
    }
    columns.emplace_back(stringBuf[3]);

    len = snprintf(stringBuf[4], sizeof(stringBuf[4]), "%f", iter.second.second);
    if (len > columnLenMax.at(4)) {
      columnLenMax.at(4) = len + 4;
    }
    columns.emplace_back(stringBuf[4]);

    rows.push_back(columns);
  }

  printf("-------------------------------------------------------------------------\n");
  for (int i = 0; i < 5; i++) {
    auto printBuf = title[i];
    if (printBuf.size() > columnLenMax.at(i)) {
      columnLenMax.at(i) = printBuf.size();
    }
    printBuf.resize(columnLenMax.at(i), ' ');
    printf("%s\t", printBuf.c_str());
  }
  printf("\n");
  for (size_t i = 0; i < rows.size(); i++) {
    for (int j = 0; j < 5; j++) {
      auto printBuf = rows[i][j];
      printBuf.resize(columnLenMax.at(j), ' ');
      printf("%s\t", printBuf.c_str());
    }
    printf("\n");
  }
  return RET_OK;
}

int TimeProfiler::RunTimeProfiler() {
  uint64_t time_avg = 0;

  // Load graph
  std::string modelName = _flags->model_path_.substr(_flags->model_path_.find_last_of("/") + 1);

  MS_LOG(INFO) << "start reading model file";
  size_t size = 0;
  char *graphBuf = ReadFile(_flags->model_path_.c_str(), &size);
  if (graphBuf == nullptr) {
    MS_LOG(ERROR) << "Load graph failed while running " << modelName.c_str();
    std::cerr << "Load graph failed while running " << modelName.c_str() << std::endl;
    delete graphBuf;
    delete session_;
    return RET_ERROR;
  }
  auto model = lite::Model::Import(graphBuf, size);
  delete graphBuf;
  if (model == nullptr) {
    MS_LOG(ERROR) << "Import model file failed while running " << modelName.c_str();
    std::cerr << "Import model file failed while running " << modelName.c_str() << std::endl;
    delete session_;
    return RET_ERROR;
  }
  auto ret = session_->CompileGraph(model);
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "Compile graph failed.";
    std::cerr << "Compile graph failed." << std::endl;
    delete session_;
    delete model;
    return RET_ERROR;
  }

  // load input
  MS_LOG(INFO) << "start generate input data";
  auto status = LoadInput();
  if (status != RET_OK) {
    MS_LOG(ERROR) << "Generate input data error";
    std::cerr << "Generate input data error" << std::endl;
    delete session_;
    delete model;
    return status;
  }

  // run graph and test
  for (int i = 0; i < _flags->loop_count_; i++) {
    session_->BindThread(true);
    uint64_t run_begin = GetTimeUs();

    ret = session_->RunGraph(before_call_back_, after_call_back_);
    if (ret != RET_OK) {
      MS_LOG(ERROR) << "Run graph failed.";
      std::cerr << "Run graph failed." << std::endl;
      delete session_;
      delete model;
      return RET_ERROR;
    }
    auto outputs = session_->GetOutputs();

    uint64_t run_end = GetTimeUs();
    uint64_t time = run_end - run_begin;
    time_avg += time;
    session_->BindThread(false);
    outputs.clear();
  }

  time_avg /= _flags->loop_count_;
  float runCost = static_cast<float>(time_avg) / 1000.0f;

  const std::vector<std::string> per_op_name = {"opName", "avg(ms)", "percent", "calledTimes", "opTotalTime"};
  const std::vector<std::string> per_op_type = {"opType", "avg(ms)", "percent", "calledTimes", "opTotalTime"};
  PrintResult(per_op_name, op_times_by_name_);
  PrintResult(per_op_type, op_times_by_type_);

  printf("\n total time:     %5.5f ms,   kernel cost:   %5.5f ms \n\n", runCost, op_cost_total_ / _flags->loop_count_);
  printf("-------------------------------------------------------------------------\n");
  delete model;
  delete session_;
  return ret;
}

int RunTimeProfiler(int argc, const char **argv) {
  TimeProfilerFlags flags;
  Option<std::string> err = flags.ParseFlags(argc, argv);

  if (err.IsSome()) {
    std::cerr << err.Get() << std::endl;
    std::cerr << flags.Usage() << std::endl;
    return -1;
  }

  if (flags.help) {
    std::cerr << flags.Usage() << std::endl;
    return 0;
  }

  TimeProfiler time_profiler(&flags);
  auto ret = time_profiler.Init();
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "Init TimeProfile failed.";
    std::cerr << "Init TimeProfile failed." << std::endl;
    return RET_ERROR;
  }

  ret = time_profiler.RunTimeProfiler();
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "Run TimeProfile failed.";
    std::cerr << "Run TimeProfile failed." << std::endl;
    return RET_ERROR;
  }

  return RET_OK;
}
}  // namespace lite
}  // namespace mindspore
