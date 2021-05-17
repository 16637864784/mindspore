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

#include "tools/benchmark_train/net_train.h"
#define __STDC_FORMAT_MACROS
#include <cinttypes>
#undef __STDC_FORMAT_MACROS
#include <algorithm>
#include <utility>
#ifdef ENABLE_NEON
#include <arm_neon.h>
#endif
#include "src/common/common.h"
#include "include/ms_tensor.h"
#include "include/context.h"
#include "src/runtime/runtime_api.h"
#include "include/version.h"
#include "include/model.h"

namespace mindspore {
namespace lite {
static const char *DELIM_SLASH = "/";

namespace {
float *ReadFileBuf(const char *file, size_t *size) {
  if (file == nullptr) {
    MS_LOG(ERROR) << "file is nullptr";
    return nullptr;
  }
  MS_ASSERT(size != nullptr);
  std::string real_path = RealPath(file);
  std::ifstream ifs(real_path);
  if (!ifs.good()) {
    MS_LOG(ERROR) << "file: " << real_path << " is not exist";
    return nullptr;
  }

  if (!ifs.is_open()) {
    MS_LOG(ERROR) << "file: " << real_path << " open failed";
    return nullptr;
  }

  ifs.seekg(0, std::ios::end);
  *size = ifs.tellg();
  std::unique_ptr<float[]> buf((new (std::nothrow) float[*size / sizeof(float) + 1]));
  if (buf == nullptr) {
    MS_LOG(ERROR) << "malloc buf failed, file: " << real_path;
    ifs.close();
    return nullptr;
  }

  ifs.seekg(0, std::ios::beg);
  ifs.read(reinterpret_cast<char *>(buf.get()), *size);
  ifs.close();

  return buf.release();
}
}  // namespace

int NetTrain::GenerateRandomData(size_t size, void *data) {
  MS_ASSERT(data != nullptr);
  char *casted_data = static_cast<char *>(data);
  for (size_t i = 0; i < size; i++) {
    casted_data[i] = static_cast<char>(i);
  }
  return RET_OK;
}

int NetTrain::GenerateInputData(std::vector<mindspore::tensor::MSTensor *> *ms_inputs) {
  for (auto tensor : *ms_inputs) {
    MS_ASSERT(tensor != nullptr);
    auto input_data = tensor->MutableData();
    if (input_data == nullptr) {
      MS_LOG(ERROR) << "MallocData for inTensor failed";
      return RET_ERROR;
    }
    auto tensor_byte_size = tensor->Size();
    auto status = GenerateRandomData(tensor_byte_size, input_data);
    if (status != RET_OK) {
      std::cerr << "GenerateRandomData for inTensor failed: " << status << std::endl;
      MS_LOG(ERROR) << "GenerateRandomData for inTensor failed:" << status;
      return status;
    }
  }
  return RET_OK;
}

int NetTrain::LoadInput(std::vector<mindspore::tensor::MSTensor *> *ms_inputs) {
  if (flags_->in_data_file_.empty()) {
    auto status = GenerateInputData(ms_inputs);
    if (status != RET_OK) {
      std::cerr << "Generate input data error " << status << std::endl;
      MS_LOG(ERROR) << "Generate input data error " << status;
      return status;
    }
  } else {
    auto status = ReadInputFile(ms_inputs);
    if (status != RET_OK) {
      std::cerr << "ReadInputFile error, " << status << std::endl;
      MS_LOG(ERROR) << "ReadInputFile error, " << status;
      return status;
    }
  }
  return RET_OK;
}

int NetTrain::ReadInputFile(std::vector<mindspore::tensor::MSTensor *> *ms_inputs) {
  if (ms_inputs->empty()) {
    return RET_OK;
  }

  if (this->flags_->in_data_type_ == kImage) {
    MS_LOG(ERROR) << "Not supported image input";
    return RET_ERROR;
  } else {
    for (size_t i = 0; i < ms_inputs->size(); i++) {
      auto cur_tensor = ms_inputs->at(i);
      MS_ASSERT(cur_tensor != nullptr);
      size_t size;
      std::string file_name = flags_->in_data_file_ + std::to_string(i + 1) + ".bin";
      char *bin_buf = ReadFile(file_name.c_str(), &size);
      if (bin_buf == nullptr) {
        MS_LOG(ERROR) << "ReadFile return nullptr";
        return RET_ERROR;
      }
      auto tensor_data_size = cur_tensor->Size();
      if (size != tensor_data_size) {
        std::cerr << "Input binary file size error, required: " << tensor_data_size << ", in fact: " << size
                  << std::endl;
        MS_LOG(ERROR) << "Input binary file size error, required: " << tensor_data_size << ", in fact: " << size;
        delete bin_buf;
        return RET_ERROR;
      }
      auto input_data = cur_tensor->MutableData();
      memcpy(input_data, bin_buf, tensor_data_size);
      delete[](bin_buf);
    }
  }
  return RET_OK;
}

int NetTrain::CompareOutput(const session::LiteSession &lite_session) {
  std::cout << "================ Comparing Forward Output data ================" << std::endl;
  float total_bias = 0;
  int total_size = 0;
  bool has_error = false;
  auto tensors_list = lite_session.GetOutputs();
  if (tensors_list.empty()) {
    MS_LOG(ERROR) << "Cannot find output tensors, get model output failed";
    return RET_ERROR;
  }
  mindspore::tensor::MSTensor *tensor = nullptr;
  int i = 1;
  for (auto it = tensors_list.begin(); it != tensors_list.end(); ++it) {
    tensor = lite_session.GetOutputByTensorName(it->first);
    std::cout << "output is tensor " << it->first << "\n";
    auto outputs = tensor->data();
    size_t size;
    std::string output_file = flags_->data_file_ + std::to_string(i) + ".bin";
    auto *bin_buf = ReadFileBuf(output_file.c_str(), &size);
    if (bin_buf == nullptr) {
      MS_LOG(ERROR) << "ReadFile return nullptr";
      return RET_ERROR;
    }
    if (size != tensor->Size()) {
      MS_LOG(ERROR) << "Output buffer and output file differ by size. Tensor size: " << tensor->Size()
                    << ", read size: " << size;
      return RET_ERROR;
    }
    float bias = CompareData<float>(bin_buf, tensor->ElementsNum(), reinterpret_cast<float *>(outputs));
    if (bias >= 0) {
      total_bias += bias;
      total_size++;
    } else {
      has_error = true;
      break;
    }
    i++;
    delete[] bin_buf;
  }

  if (!has_error) {
    float mean_bias;
    if (total_size != 0) {
      mean_bias = total_bias / total_size * 100;
    } else {
      mean_bias = 0;
    }

    std::cout << "Mean bias of all nodes/tensors: " << mean_bias << "%"
              << " threshold is:" << this->flags_->accuracy_threshold_ << std::endl;
    std::cout << "=======================================================" << std::endl << std::endl;

    if (mean_bias > this->flags_->accuracy_threshold_) {
      MS_LOG(ERROR) << "Mean bias of all nodes/tensors is too big: " << mean_bias << "%";
      std::cerr << "Mean bias of all nodes/tensors is too big: " << mean_bias << "%" << std::endl;
      return RET_ERROR;
    } else {
      return RET_OK;
    }
  } else {
    MS_LOG(ERROR) << "Error in CompareData";
    std::cerr << "Error in CompareData" << std::endl;
    std::cout << "=======================================================" << std::endl << std::endl;
    return RET_ERROR;
  }
}

int NetTrain::MarkPerformance(session::TrainSession *session) {
  MS_LOG(INFO) << "Running train loops...";
  std::cout << "Running train loops..." << std::endl;
  uint64_t time_min = 0xFFFFFFFFFFFFFFFF;
  uint64_t time_max = 0;
  uint64_t time_avg = 0;

  for (int i = 0; i < flags_->epochs_; i++) {
    session->BindThread(true);
    auto start = GetTimeUs();
    auto status =
      flags_->time_profiling_ ? session->RunGraph(before_call_back_, after_call_back_) : session->RunGraph();
    if (status != 0) {
      MS_LOG(ERROR) << "Inference error " << status;
      std::cerr << "Inference error " << status;
      return status;
    }

    auto end = GetTimeUs();
    auto time = end - start;
    time_min = std::min(time_min, time);
    time_max = std::max(time_max, time);
    time_avg += time;
    session->BindThread(false);
  }

  if (flags_->time_profiling_) {
    const std::vector<std::string> per_op_name = {"opName", "avg(ms)", "percent", "calledTimes", "opTotalTime"};
    const std::vector<std::string> per_op_type = {"opType", "avg(ms)", "percent", "calledTimes", "opTotalTime"};
    PrintResult(per_op_name, op_times_by_name_);
    PrintResult(per_op_type, op_times_by_type_);
  }

  if (flags_->epochs_ > 0) {
    time_avg /= flags_->epochs_;
    MS_LOG(INFO) << "Model = " << flags_->model_file_.substr(flags_->model_file_.find_last_of(DELIM_SLASH) + 1).c_str()
                 << ", NumThreads = " << flags_->num_threads_ << ", MinRunTime = " << time_min / 1000.0f
                 << ", MaxRuntime = " << time_max / 1000.0f << ", AvgRunTime = " << time_avg / 1000.0f;
    printf("Model = %s, NumThreads = %d, MinRunTime = %f ms, MaxRuntime = %f ms, AvgRunTime = %f ms\n",
           flags_->model_file_.substr(flags_->model_file_.find_last_of(DELIM_SLASH) + 1).c_str(), flags_->num_threads_,
           time_min / 1000.0f, time_max / 1000.0f, time_avg / 1000.0f);
  }
  return RET_OK;
}

int NetTrain::MarkAccuracy(session::LiteSession *session) {
  MS_LOG(INFO) << "MarkAccuracy";
  for (auto &msInput : session->GetInputs()) {
    switch (msInput->data_type()) {
      case TypeId::kNumberTypeFloat:
        PrintInputData<float>(msInput);
        break;
      case TypeId::kNumberTypeFloat32:
        PrintInputData<float>(msInput);
        break;
      case TypeId::kNumberTypeInt32:
        PrintInputData<int>(msInput);
        break;
      default:
        MS_LOG(ERROR) << "Datatype " << msInput->data_type() << " is not supported.";
        return RET_ERROR;
    }
  }
  auto status = session->RunGraph();
  if (status != RET_OK) {
    MS_LOG(ERROR) << "Inference error " << status;
    std::cerr << "Inference error " << status << std::endl;
    return status;
  }

  status = CompareOutput(*session);
  if (status != RET_OK) {
    MS_LOG(ERROR) << "Compare output error " << status;
    std::cerr << "Compare output error " << status << std::endl;
    return status;
  }
  return RET_OK;
}

static CpuBindMode FlagToBindMode(int flag) {
  if (flag == 2) {
    return MID_CPU;
  }
  if (flag == 1) {
    return HIGHER_CPU;
  }
  return NO_BIND;
}

int NetTrain::CreateAndRunNetwork(const std::string &filename, int train_session, int epochs) {
  auto start_prepare_time = GetTimeUs();
  std::string model_name = filename.substr(filename.find_last_of(DELIM_SLASH) + 1);
  Context context;
  context.device_list_[0].device_info_.cpu_device_info_.cpu_bind_mode_ = FlagToBindMode(flags_->cpu_bind_mode_);
  context.device_list_[0].device_info_.cpu_device_info_.enable_float16_ = flags_->enable_fp16_;
  context.device_list_[0].device_type_ = mindspore::lite::DT_CPU;
  context.thread_num_ = flags_->num_threads_;

  TrainCfg train_cfg;
  if (flags_->loss_name_ != "") {
    train_cfg.loss_name_ = flags_->loss_name_;
  }

  session::LiteSession *session = nullptr;
  session::TrainSession *t_session = nullptr;
  if (train_session) {
    MS_LOG(INFO) << "CreateSession from model file" << filename.c_str();
    std::cout << "CreateSession from model file " << filename.c_str() << std::endl;
    t_session = session::TrainSession::CreateSession(filename, &context, true, &train_cfg);
    if (t_session == nullptr) {
      MS_LOG(ERROR) << "RunNetTrain CreateSession failed while running " << model_name.c_str();
      std::cout << "RunNetTrain CreateSession failed while running " << model_name.c_str() << std::endl;
      return RET_ERROR;
    }

    if (epochs > 0) {
      t_session->Train();
    }
    session = t_session;
  } else {
    MS_LOG(INFO) << "start reading model file" << filename.c_str();
    std::cout << "start reading model file " << filename.c_str() << std::endl;
    auto *model = mindspore::lite::Model::Import(filename.c_str());
    if (model == nullptr) {
      MS_LOG(ERROR) << "create model for train session failed";
      return RET_ERROR;
    }
    session = session::LiteSession::CreateSession(&context);
    if (session == nullptr) {
      MS_LOG(ERROR) << "ExportedFile CreateSession failed while running " << model_name.c_str();
      std::cout << "CreateSession failed while running " << model_name.c_str() << std::endl;
      delete model;
      return RET_ERROR;
    }
    if (session->CompileGraph(model) != RET_OK) {
      MS_LOG(ERROR) << "Cannot compile model";
      delete model;
      return RET_ERROR;
    }
    delete model;
  }

  auto end_prepare_time = GetTimeUs();
  MS_LOG(INFO) << "PrepareTime = " << (end_prepare_time - start_prepare_time) / 1000 << " ms";
  std::cout << "PrepareTime = " << (end_prepare_time - start_prepare_time) / 1000 << " ms" << std::endl;
  // Load input
  MS_LOG(INFO) << "Load input data";
  auto ms_inputs = session->GetInputs();
  auto status = LoadInput(&ms_inputs);
  if (status != RET_OK) {
    MS_LOG(ERROR) << "Load input data error";
    return status;
  }

  if ((epochs > 0) && (t_session != nullptr)) {
    status = MarkPerformance(t_session);
    if (status != RET_OK) {
      MS_LOG(ERROR) << "Run MarkPerformance error: " << status;
      std::cout << "Run MarkPerformance error: " << status << std::endl;
      return status;
    }
    SaveModels(t_session);  // save file if flags are on
  }
  if (!flags_->data_file_.empty()) {
    if (t_session != nullptr) {
      t_session->Eval();
    }
    status = MarkAccuracy(session);
    if (status != RET_OK) {
      MS_LOG(ERROR) << "Run MarkAccuracy error: " << status;
      std::cout << "Run MarkAccuracy error: " << status << std::endl;
      return status;
    }
  }
  return RET_OK;
}

int NetTrain::RunNetTrain() {
  CreateAndRunNetwork(flags_->model_file_, true, flags_->epochs_);

  auto status = CheckExecutionOfSavedModels();  // re-initialize sessions according to flags
  if (status != RET_OK) {
    MS_LOG(ERROR) << "Run CheckExecute error: " << status;
    std::cout << "Run CheckExecute error: " << status << std::endl;
    return status;
  }
  return RET_OK;
}

int NetTrain::SaveModels(session::TrainSession *session) {
  if (!flags_->export_file_.empty()) {
    auto status = session->Export(flags_->export_file_);
    if (status != RET_OK) {
      MS_LOG(ERROR) << "SaveToFile error";
      std::cout << "Run SaveToFile error";
      return RET_ERROR;
    }
  }
  if (!flags_->inference_file_.empty()) {
    auto tick = GetTimeUs();
    auto status = session->Export(flags_->inference_file_, lite::MT_INFERENCE);
    if (status != RET_OK) {
      MS_LOG(ERROR) << "Save model error: " << status;
      std::cout << "Save model error: " << status << std::endl;
      return status;
    }
    std::cout << "ExportInference() execution time is " << GetTimeUs() - tick << "us\n";
  }
  return RET_OK;
}

int NetTrain::CheckExecutionOfSavedModels() {
  int status = RET_OK;
  if (!flags_->export_file_.empty()) {
    status = NetTrain::CreateAndRunNetwork(flags_->export_file_, true, 0);
    if (status != RET_OK) {
      MS_LOG(ERROR) << "Run Exported model " << flags_->export_file_ << " error: " << status;
      std::cout << "Run Exported model " << flags_->export_file_ << " error: " << status << std::endl;
      return status;
    }
  }
  if (!flags_->inference_file_.empty()) {
    status = NetTrain::CreateAndRunNetwork(flags_->inference_file_ + ".ms", false, 0);
    if (status != RET_OK) {
      MS_LOG(ERROR) << "Running saved model " << flags_->inference_file_ << ".ms error: " << status;
      std::cout << "Running saved model " << flags_->inference_file_ << ".ms error: " << status << std::endl;
      return status;
    }
  }
  return status;
}

int NetTrain::InitCallbackParameter() {
  // before callback
  before_call_back_ = [&](const std::vector<mindspore::tensor::MSTensor *> &before_inputs,
                          const std::vector<mindspore::tensor::MSTensor *> &before_outputs,
                          const mindspore::CallBackParam &callParam) {
    if (before_inputs.empty()) {
      MS_LOG(INFO) << "The num of beforeInputs is empty";
    }
    if (before_outputs.empty()) {
      MS_LOG(INFO) << "The num of beforeOutputs is empty";
    }
    if (op_times_by_type_.find(callParam.node_type) == op_times_by_type_.end()) {
      op_times_by_type_.insert(std::make_pair(callParam.node_type, std::make_pair(0, 0.0f)));
    }
    if (op_times_by_name_.find(callParam.node_name) == op_times_by_name_.end()) {
      op_times_by_name_.insert(std::make_pair(callParam.node_name, std::make_pair(0, 0.0f)));
    }
    op_call_times_total_++;
    op_begin_ = GetTimeUs();
    return true;
  };

  // after callback
  after_call_back_ = [&](const std::vector<mindspore::tensor::MSTensor *> &after_inputs,
                         const std::vector<mindspore::tensor::MSTensor *> &after_outputs,
                         const mindspore::CallBackParam &call_param) {
    uint64_t opEnd = GetTimeUs();

    if (after_inputs.empty()) {
      MS_LOG(INFO) << "The num of after inputs is empty";
    }
    if (after_outputs.empty()) {
      MS_LOG(INFO) << "The num of after outputs is empty";
    }

    float cost = static_cast<float>(opEnd - op_begin_) / 1000.0f;
    op_cost_total_ += cost;
    op_times_by_type_[call_param.node_type].first++;
    op_times_by_type_[call_param.node_type].second += cost;
    op_times_by_name_[call_param.node_name].first++;
    op_times_by_name_[call_param.node_name].second += cost;
    if (flags_->layer_checksum_) {
      auto out_tensor = after_outputs.at(0);
      void *output = out_tensor->MutableData();
      int tensor_size = out_tensor->ElementsNum();
      TypeId type = out_tensor->data_type();
      std::cout << call_param.node_type << " shape=" << after_outputs.at(0)->shape() << " sum=";
      switch (type) {
        case kNumberTypeFloat32:
          std::cout << TensorSum<float>(output, tensor_size);
          break;
        case kNumberTypeInt32:
          std::cout << TensorSum<int>(output, tensor_size);
          break;
#ifdef ENABLE_FP16
        case kNumberTypeFloat16:
          std::cout << TensorSum<float16_t>(output, tensor_size);
          break;
#endif
        default:
          std::cout << "unsupported type:" << type;
          break;
      }
      std::cout << std::endl;
    }
    return true;
  };
  return RET_OK;
}

int NetTrain::Init() {
  if (this->flags_ == nullptr) {
    return 1;
  }
  MS_LOG(INFO) << "ModelPath = " << this->flags_->model_file_;
  MS_LOG(INFO) << "InDataPath = " << this->flags_->in_data_file_;
  MS_LOG(INFO) << "InDataType = " << this->flags_->in_data_type_in_;
  MS_LOG(INFO) << "Epochs = " << this->flags_->epochs_;
  MS_LOG(INFO) << "AccuracyThreshold = " << this->flags_->accuracy_threshold_;
  MS_LOG(INFO) << "WarmUpLoopCount = " << this->flags_->warm_up_loop_count_;
  MS_LOG(INFO) << "NumThreads = " << this->flags_->num_threads_;
  MS_LOG(INFO) << "expectedDataFile = " << this->flags_->data_file_;
  MS_LOG(INFO) << "exportDataFile = " << this->flags_->export_file_;
  MS_LOG(INFO) << "enableFp16 = " << this->flags_->enable_fp16_;

  if (this->flags_->epochs_ < 0) {
    MS_LOG(ERROR) << "epochs:" << this->flags_->epochs_ << " must be equal/greater than 0";
    std::cerr << "epochs:" << this->flags_->epochs_ << " must be equal/greater than 0" << std::endl;
    return RET_ERROR;
  }

  if (this->flags_->num_threads_ < 1) {
    MS_LOG(ERROR) << "numThreads:" << this->flags_->num_threads_ << " must be greater than 0";
    std::cerr << "numThreads:" << this->flags_->num_threads_ << " must be greater than 0" << std::endl;
    return RET_ERROR;
  }

  this->flags_->in_data_type_ = this->flags_->in_data_type_in_ == "img" ? kImage : kBinary;

  if (flags_->in_data_file_.empty() && !flags_->data_file_.empty()) {
    MS_LOG(ERROR) << "expectedDataFile not supported in case that inDataFile is not provided";
    std::cerr << "expectedDataFile is not supported in case that inDataFile is not provided" << std::endl;
    return RET_ERROR;
  }

  if (flags_->in_data_file_.empty() && !flags_->export_file_.empty()) {
    MS_LOG(ERROR) << "exportDataFile not supported in case that inDataFile is not provided";
    std::cerr << "exportDataFile is not supported in case that inDataFile is not provided" << std::endl;
    return RET_ERROR;
  }

  if (flags_->model_file_.empty()) {
    MS_LOG(ERROR) << "modelPath is required";
    std::cerr << "modelPath is required" << std::endl;
    return 1;
  }

  if (flags_->time_profiling_) {
    auto status = InitCallbackParameter();
    if (status != RET_OK) {
      MS_LOG(ERROR) << "Init callback Parameter failed.";
      std::cerr << "Init callback Parameter failed." << std::endl;
      return RET_ERROR;
    }
  }

  return RET_OK;
}

int NetTrain::PrintResult(const std::vector<std::string> &title,
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

    len = snprintf(stringBuf[1], sizeof(stringBuf[1]), "%f", iter.second.second / flags_->epochs_);
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

int RunNetTrain(int argc, const char **argv) {
  NetTrainFlags flags;
  Option<std::string> err = flags.ParseFlags(argc, argv);

  if (err.IsSome()) {
    std::cerr << err.Get() << std::endl;
    std::cerr << flags.Usage() << std::endl;
    return RET_ERROR;
  }

  if (flags.help) {
    std::cerr << flags.Usage() << std::endl;
    return RET_OK;
  }

  NetTrain net_trainer(&flags);
  auto status = net_trainer.Init();
  if (status != RET_OK) {
    MS_LOG(ERROR) << "NetTrain init Error : " << status;
    std::cerr << "NetTrain init Error : " << status << std::endl;
    return RET_ERROR;
  }

  status = net_trainer.RunNetTrain();
  if (status != RET_OK) {
    MS_LOG(ERROR) << "Run NetTrain "
                  << flags.model_file_.substr(flags.model_file_.find_last_of(DELIM_SLASH) + 1).c_str()
                  << " Failed : " << status;
    std::cerr << "Run NetTrain " << flags.model_file_.substr(flags.model_file_.find_last_of(DELIM_SLASH) + 1).c_str()
              << " Failed : " << status << std::endl;
    return RET_ERROR;
  }

  MS_LOG(INFO) << "Run NetTrain " << flags.model_file_.substr(flags.model_file_.find_last_of(DELIM_SLASH) + 1).c_str()
               << " Success.";
  std::cout << "Run NetTrain " << flags.model_file_.substr(flags.model_file_.find_last_of(DELIM_SLASH) + 1).c_str()
            << " Success." << std::endl;
  return RET_OK;
}
}  // namespace lite
}  // namespace mindspore
