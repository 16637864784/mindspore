/**
 * Copyright 2019 Huawei Technologies Co., Ltd
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

#include "tools/converter/quantizer/post_training_quantizer.h"
#include <dirent.h>
#include <sys/stat.h>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <memory>
#include <numeric>
#include <utility>
#include <string>
#include <vector>
#include <fstream>
#include "schema/inner/model_generated.h"
#include "src/tensor.h"
#include "tools/anf_exporter/anf_exporter.h"
#include "tools/converter/quantizer/quantize_util.h"
#include "utils/log_adapter.h"
#include "securec/include/securec.h"
#include "tools/common/tensor_util.h"
#include "src/common/file_utils.h"

using std::string;
using std::vector;

namespace mindspore {
namespace lite {
namespace quant {
STATUS DivergInfo::RecordMaxValue(const std::vector<float> &datas) {
  for (float data : datas) {
    max = std::max(data, max);
    min = std::min(data, min);
  }
  return RET_OK;
}

void DivergInfo::UpdateInterval() {
  auto max_value = std::max(fabs(this->max), fabs(this->min));
  this->interval = max_value / static_cast<float>(bin_num);
}

STATUS DivergInfo::UpdateHistogram(const std::vector<float> &data) {
  for (auto value : data) {
    if (value == 0) {
      continue;
    }
    int bin_index = std::min(static_cast<int>(std::fabs(value) / this->interval), bin_num - 1);
    this->histogram[bin_index]++;
  }
  return RET_OK;
}

void DivergInfo::DumpHistogram() {
  MS_LOG(INFO) << "Print node " << cnode->fullname_with_scope() << " histogram";
  for (float item : this->histogram) {
    std::cout << item << " ";
  }
  std::cout << std::endl;
}

STATUS DivergInfo::ComputeThreshold() {
  if (method_x == kMethodMaxMin) {
    this->best_T = std::max(fabs(this->max), fabs(this->min));
    MS_LOG(DEBUG) << "using MAX_MIN, T: " << this->best_T;
    return RET_OK;
  }

  constexpr int quant_bint_nums = 128;
  int threshold = quant_bint_nums;
  float min_kl = FLT_MAX;
  float after_threshold_sum = std::accumulate(this->histogram.begin() + quant_bint_nums, this->histogram.end(), 0.0f);

  for (int i = quant_bint_nums; i < this->bin_num; ++i) {
    std::vector<float> quantized_histogram(quant_bint_nums, 0);
    std::vector<float> reference_histogram(this->histogram.begin(), this->histogram.begin() + i);
    std::vector<float> expanded_histogram(i, 0);
    reference_histogram[i - 1] += after_threshold_sum;
    after_threshold_sum -= this->histogram[i];

    const float bin_interval = static_cast<float>(i) / static_cast<float>(quant_bint_nums);

    // merge i bins to target bins
    for (int j = 0; j < quant_bint_nums; ++j) {
      const float start = j * bin_interval;
      const float end = start + bin_interval;
      const int left_upper = static_cast<int>(std::ceil(start));
      if (left_upper > start) {
        const double left_scale = left_upper - start;
        quantized_histogram[j] += left_scale * this->histogram[left_upper - 1];
      }
      const int right_lower = static_cast<int>(std::floor(end));
      if (right_lower < end) {
        const double right_scale = end - right_lower;
        quantized_histogram[j] += right_scale * this->histogram[right_lower];
      }
      std::for_each(this->histogram.begin() + left_upper, this->histogram.begin() + right_lower,
                    [&quantized_histogram, j](float item) { quantized_histogram[j] += item; });
    }
    // expand target bins to i bins in order to calculate KL with reference_histogram
    for (int j = 0; j < quant_bint_nums; ++j) {
      const float start = j * bin_interval;
      const float end = start + bin_interval;
      float count = 0;
      const int left_upper = static_cast<int>(std::ceil(start));
      float left_scale = 0.0f;
      if (left_upper > start) {
        left_scale = left_upper - start;
        if (this->histogram[left_upper - 1] != 0) {
          count += left_scale;
        }
      }
      const int right_lower = static_cast<int>(std::floor(end));
      double right_scale = 0.0f;
      if (right_lower < end) {
        right_scale = end - right_lower;
        if (this->histogram[right_lower] != 0) {
          count += right_scale;
        }
      }
      std::for_each(this->histogram.begin() + left_upper, this->histogram.begin() + right_lower, [&count](float item) {
        if (item != 0) {
          count += 1;
        }
      });
      if (count == 0) {
        continue;
      }
      const float average_num = quantized_histogram[j] / count;
      if (left_upper > start && this->histogram[left_upper - 1] != 0) {
        expanded_histogram[left_upper - 1] += average_num * left_scale;
      }
      if (right_lower < end && this->histogram[right_lower] != 0) {
        expanded_histogram[right_lower] += average_num * right_scale;
      }
      for (int k = left_upper; k < right_lower; ++k) {
        if (this->histogram[k] != 0) {
          expanded_histogram[k] += average_num;
        }
      }
    }
    auto KLDivergence = [](std::vector<float> p, std::vector<float> q) {
      auto sum = 0.0f;
      std::for_each(p.begin(), p.end(), [&sum](float item) { sum += item; });
      std::for_each(p.begin(), p.end(), [sum](float &item) { item /= sum; });
      sum = 0.0f;
      std::for_each(q.begin(), q.end(), [&sum](float item) { sum += item; });
      std::for_each(q.begin(), q.end(), [sum](float &item) { item /= sum; });

      float result = 0.0f;
      const int size = p.size();
      for (int i = 0; i < size; ++i) {
        if (p[i] != 0) {
          if (q[i] == 0) {
            result += 1.0f;
          } else {
            result += (p[i] * std::log((p[i]) / (q[i])));
          }
        }
      }
      return result;
    };
    const float kl = KLDivergence(reference_histogram, expanded_histogram);
    if (kl < min_kl) {
      min_kl = kl;
      threshold = i;
    }
  }
  this->best_T = (static_cast<float>(threshold) + 0.5f) * this->interval;
  MS_LOG(DEBUG) << cnode->fullname_with_scope() << " Best threshold bin index: " << threshold << " T: " << best_T
                << " max: " << std::max(fabs(this->max), fabs(this->min));
  return RET_OK;
}

std::pair<CNodePtr, float> DivergInfo::GetScale() {
  float max_value = this->best_T;
  float min_value = -max_value;

  MS_ASSERT(quant_max - quant_min != 0);
  float scale = (max_value - min_value) / (quant_max - quant_min);
  MS_ASSERT(scale != 0);
  return std::make_pair(this->cnode, scale);
}

std::pair<CNodePtr, int32_t> DivergInfo::GetZeropoint() {
  int zero_point = 0;
  if (quant_min == 0 && quant_max == 255) {
    zero_point = 128;
  } else if (quant_min == -127 && quant_max == 127) {
    zero_point = 0;
  } else {
    MS_LOG(WARNING) << "unexpectd quant range, quant_min: " << quant_min << " quant_max: " << quant_max;
  }
  return std::make_pair(this->cnode, zero_point);
}

std::unordered_map<CNodePtr, float> Calibrator::GetScale(
  std::unordered_map<std::string, std::unique_ptr<DivergInfo>> *diverg_info) {
  std::unordered_map<CNodePtr, float> result;
  for (auto iter = diverg_info->begin(); iter != diverg_info->end(); iter++) {
    DivergInfo *info = iter->second.get();
    auto item = info->GetScale();
    result.insert(item);
  }
  return result;
}
std::unordered_map<CNodePtr, int32_t> Calibrator::GetZeropoint(
  std::unordered_map<std::string, std::unique_ptr<DivergInfo>> *diverg_info) {
  std::unordered_map<CNodePtr, int32_t> result;
  for (auto iter = diverg_info->begin(); iter != diverg_info->end(); iter++) {
    DivergInfo *info = iter->second.get();
    auto zeropoint = info->GetZeropoint();
    result.insert(zeropoint);
  }
  return result;
}

std::map<CNodePtr, MaxMin> Calibrator::GetMinMax(
  std::unordered_map<std::string, std::unique_ptr<DivergInfo>> *diverg_info) {
  std::map<CNodePtr, MaxMin> result;
  for (auto iter = diverg_info->begin(); iter != diverg_info->end(); iter++) {
    DivergInfo *info = iter->second.get();
    mindspore::lite::quant::MaxMin input_maxmin{};
    input_maxmin.min = info->min;
    input_maxmin.max = info->max;
    result[info->cnode] = input_maxmin;
  }
  return result;
}

void Calibrator::Dump() {
  for (auto iter = this->input_diverg_info_.begin(); iter != this->input_diverg_info_.end(); iter++) {
    DivergInfo *info = iter->second.get();
    info->DumpHistogram();
  }
}

std::unordered_map<std::string, std::unique_ptr<DivergInfo>> *Calibrator::GetInputDivergInfo() {
  return &this->input_diverg_info_;
}

std::unordered_map<std::string, std::unique_ptr<DivergInfo>> *Calibrator::GetOutputDivergInfo() {
  return &this->output_diverg_info_;
}

STATUS Calibrator::RecordMaxValue(const std::string &op_name, const vector<float> &data,
                                  std::unordered_map<std::string, std::unique_ptr<DivergInfo>> *diverg_info) {
  auto got = (*diverg_info).find(op_name);
  if (got != (*diverg_info).end()) {
    ((*got).second)->RecordMaxValue(data);
  }
  return RET_OK;
}

STATUS Calibrator::ComputeThreshold() {
  for (auto iter = this->output_diverg_info_.begin(); iter != this->output_diverg_info_.end(); iter++) {
    DivergInfo *info = iter->second.get();
    info->ComputeThreshold();
  }
  // node A's input may be node B's output, no need to re-compute the node A's input quant param which is the same as
  for (auto iter = this->input_diverg_info_.begin(); iter != this->input_diverg_info_.end(); iter++) {
    DivergInfo *info = iter->second.get();
    auto cnode = info->cnode;

    bool already_computed = false;
    auto input = cnode->input(1);
    if (input->isa<mindspore::CNode>()) {
      auto input_cnode = std::dynamic_pointer_cast<mindspore::CNode>(input);
      for (const auto &output_diverg_info : output_diverg_info_) {
        auto output_diverg_cnode = output_diverg_info.second->cnode;
        if (output_diverg_cnode == input_cnode) {
          *info = *(output_diverg_info.second);
          info->cnode = cnode;
          already_computed = true;
          break;
        }
      }
    }
    if (!already_computed) {
      info->ComputeThreshold();
    }
  }
  return RET_OK;
}

STATUS Calibrator::UpdateDivergInverval(std::unordered_map<std::string, std::unique_ptr<DivergInfo>> *diverg_info) {
  for (auto iter = (*diverg_info).begin(); iter != (*diverg_info).end(); iter++) {
    DivergInfo *info = iter->second.get();
    info->UpdateInterval();
  }
  return RET_OK;
}

STATUS Calibrator::UpdateDataFrequency(const std::string &op_name, const vector<float> &data,
                                       std::unordered_map<std::string, std::unique_ptr<DivergInfo>> *diverg_info) {
  auto got = (*diverg_info).find(op_name);
  if (got != (*diverg_info).end()) {
    ((*got).second)->UpdateHistogram(data);
  }
  return RET_OK;
}

STATUS Calibrator::AddQuantizedOp(CNodePtr node) {
  if (node == nullptr) {
    MS_LOG(ERROR) << "To be quantized node is null";
    return RET_ERROR;
  }
  string node_name = node->fullname_with_scope();
  std::unique_ptr<DivergInfo> input_diverg = std::unique_ptr<DivergInfo>(
    new DivergInfo(node, kDefaultBinNumber, bit_num_, quant_max_, quant_min_, config_param_.method_x));
  std::unique_ptr<DivergInfo> output_diverg = std::unique_ptr<DivergInfo>(
    new DivergInfo(node, kDefaultBinNumber, bit_num_, quant_max_, quant_min_, config_param_.method_x));

  input_diverg_info_.insert(std::make_pair(string(node_name), std::move(input_diverg)));
  output_diverg_info_.insert(std::make_pair(string(node_name), std::move(output_diverg)));
  return RET_OK;
}

void Calibrator::AddImage(const string file) {
  auto exist = [](const string file) {
    struct stat buf {};
    return stat(file.c_str(), &buf) == 0;
  };
  if (exist(file)) {
    MS_LOG(INFO) << "load image: " << file;
    this->images_.push_back(file);
  } else {
    MS_LOG(WARNING) << "invalid image file path: " << file;
  }
}

STATUS Calibrator::GenerateInputData(int index, mindspore::tensor::MSTensor *tensor) const {
  string path = images_[index];
  MS_LOG(INFO) << "read image: " << path;
  size_t size;
  char *bin_buf = ReadFile(path.c_str(), &size);
  auto data = tensor->MutableData();
  if (data == nullptr) {
    MS_LOG(ERROR) << "Get tensor MutableData return nullptr";
    return RET_ERROR;
  }
  if (size != tensor->Size()) {
    MS_LOG(ERROR) << "the input data is not consistent with model input, file_size: " << size
                  << " input tensor size: " << tensor->Size();
    return RET_ERROR;
  }
  auto ret = memcpy_s(data, tensor->Size(), bin_buf, size);
  if (ret != EOK) {
    MS_LOG(ERROR) << "memcpy_s error: " << ret;
    return RET_ERROR;
  }
  delete[] bin_buf;
  return RET_OK;
}

STATUS Calibrator::CollectImages() {
  // check image file path
  DIR *root = opendir(config_param_.image_path.c_str());
  if (root == nullptr) {
    MS_LOG(ERROR) << "invalid image path: " << config_param_.image_path;
    return RET_PARAM_INVALID;
  }
  struct dirent *image_dir = readdir(root);
  size_t count = 0;
  while (image_dir != nullptr) {
    if (image_dir->d_name[0] != '.') {
      const std::string file_name = config_param_.image_path + "/" + image_dir->d_name;
      if (config_param_.batch_count == 0) {
        this->AddImage(file_name);
        count++;
      } else if (count < config_param_.batch_count) {
        this->AddImage(file_name);
        count++;
      } else {
        break;
      }
    }
    image_dir = readdir(root);
  }
  closedir(root);
  return RET_OK;
}

STATUS Calibrator::ReadConfig() {
  if (config_path_.empty() || config_path_.length() > PATH_MAX) {
    MS_LOG(ERROR) << "invalid config path!";
    return RET_PARAM_INVALID;
  }
  // check whether config file path is valid
  char *resolved_path = new (std::nothrow) char[PATH_MAX]{0};
  if (resolved_path == nullptr) {
    MS_LOG(ERROR) << "New an object failed.";
    return RET_ERROR;
  }
#ifdef _WIN32
  if (_fullpath(resolved_path, config_path_.c_str(), 1024) != nullptr) {
    config_path_ = string(resolved_path);
  }
#else
  if (realpath(config_path_.c_str(), resolved_path) != nullptr) {
    config_path_ = string(resolved_path);
  }
#endif
  std::ifstream fs(config_path_.c_str(), std::ifstream::in);
  if (!fs.is_open()) {
    MS_LOG(ERROR) << "config proto file %s open failed: " << config_path_;
    delete[] resolved_path;
    return RET_PARAM_INVALID;
  }
  std::string line;
  while (std::getline(fs, line)) {
    auto index = line.find('=');
    if (index == std::string::npos) {
      MS_LOG(ERROR) << "the config file is invalid, can not find '=', please check";
      delete[] resolved_path;
      return RET_PARAM_INVALID;
    }
    auto key = line.substr(0, index);
    auto value = line.substr(index + 1);
    if (key == "image_path") {
      config_param_.image_path = value;
    } else if (key == "batch_count") {
      config_param_.batch_count = std::stoul(value);
    } else if (key == "thread_num") {
      config_param_.thread_num = std::stoul(value);
    } else if (key == "method_x") {
      if (value != kMethodKL && value != kMethodMaxMin) {
        MS_LOG(WARNING) << "unsupported method_x: " << value << ". Use default value.";
      } else {
        config_param_.method_x = value;
      }
    } else {
      MS_LOG(WARNING) << "unsupported parameter";
    }
  }
  MS_LOG(DEBUG) << "image_path: " << config_param_.image_path << "  "
                << "batch_count: " << config_param_.batch_count << "  "
                << "method_x: " << config_param_.method_x << "  "
                << "thread_num: " << config_param_.thread_num;

  delete[] resolved_path;
  fs.close();
  return RET_OK;
}

Calibrator::Calibrator(string path, size_t bit_num, int quant_max, int quant_min)
    : config_path_(path), bit_num_(bit_num), quant_max_(quant_max), quant_min_(quant_min) {}

PostTrainingQuantizer::PostTrainingQuantizer(FuncGraphPtr graph, string path, int bit_num, TypeId target_type,
                                             bool per_channel)
    : Quantizer(graph) {
  this->per_channel_ = per_channel;
  this->bit_num = bit_num;
  this->target_type_ = target_type;
  if (target_type == kNumberTypeInt8) {
    quant_max = (1 << (this->bit_num - 1)) - 1;  // 127
    quant_min = -quant_max;                      // -127
  } else if (target_type == kNumberTypeUInt8) {
    quant_max = (1 << this->bit_num) - 1;  // 255
    quant_min = 0;
  } else {
    MS_LOG(ERROR) << "unsupported quant value type: " << target_type;
  }
  calibrator_ = std::unique_ptr<Calibrator>(new Calibrator(path, this->bit_num, quant_max, quant_min));
  if (calibrator_ == nullptr) {
    MS_LOG(ERROR) << "creat calibrator failed!";
    return;
  }
}

STATUS PostTrainingQuantizer::DoQuantInput(double scale, int zeropoint, struct MaxMin *max_min,
                                           std::shared_ptr<PrimitiveC> lite_primitive) {
  if (!lite_primitive->GetInputQuantParams().empty()) {
    return RET_OK;
  }
  schema::QuantParamT quant_param;
  quant_param.scale = scale;
  quant_param.zeroPoint = zeropoint;
  quant_param.max = max_min->max;
  quant_param.min = max_min->min;
  quant_param.numBits = bit_num;
  quant_param.narrowRange = false;
  std::vector<schema::QuantParamT> quant_params = {quant_param};
  lite_primitive->AddInputQuantParam(quant_params);
  return RET_OK;
}

STATUS PostTrainingQuantizer::DoQuantOutput(double scale, int zeropoint, struct MaxMin *max_min,
                                            std::shared_ptr<PrimitiveC> lite_primitive) {
  if (!lite_primitive->GetOutputQuantParams().empty()) {
    return RET_OK;
  }
  schema::QuantParamT quant_param;
  quant_param.scale = scale;
  quant_param.zeroPoint = zeropoint;
  quant_param.max = max_min->max;
  quant_param.min = max_min->min;
  quant_param.numBits = bit_num;
  quant_param.narrowRange = false;
  std::vector<schema::QuantParamT> quant_params = {quant_param};
  lite_primitive->AddOutputQuantParam(quant_params);
  return RET_OK;
}

STATUS PostTrainingQuantizer::DoWeightQuant(AnfNodePtr weight, std::shared_ptr<PrimitiveC> primitive_c,
                                            bool perchanel) {
  // perlayer
  if (!weight->isa<Parameter>()) {
    MS_LOG(ERROR) << "not a parameter";
    return RET_PARAM_INVALID;
  }
  auto parameter = std::dynamic_pointer_cast<Parameter>(weight);
  if (parameter == nullptr) {
    MS_LOG(ERROR) << weight->fullname_with_scope() << " can not cast to Parameter";
    return RET_ERROR;
  }
  ParamValueLitePtr paramValue = std::dynamic_pointer_cast<ParamValueLite>(parameter->default_param());
  if (paramValue == nullptr) {
    MS_LOG(ERROR) << weight->fullname_with_scope() << " can not get value";
    return RET_ERROR;
  }
  auto status = QuantFilter<int8_t>(paramValue, primitive_c, QuantType_PostTraining, quant_max, quant_min, bit_num,
                                    perchanel);
  if (status != RET_OK) {
    MS_LOG(ERROR) << "QuantFilter failed: " << status;
    return status;
  }
  // set dtype
  auto abstractBase = parameter->abstract();
  if (abstractBase == nullptr) {
    MS_LOG(ERROR) << "Abstract of parameter is nullptr, " << parameter->name();
    return RET_ERROR;
  }
  if (!utils::isa<abstract::AbstractTensorPtr>(abstractBase)) {
    MS_LOG(ERROR) << "Abstract of parameter should be anstract tensor, " << parameter->name();
    return RET_ERROR;
  }
  auto abstractTensor = utils::cast<abstract::AbstractTensorPtr>(abstractBase);
  abstractTensor->element()->set_type(TypeIdToType(kNumberTypeInt8));
  return RET_OK;
}

STATUS PostTrainingQuantizer::DoBiasQuant(AnfNodePtr bias, std::shared_ptr<PrimitiveC> primitive_c) {
  if (primitive_c == nullptr || bias == nullptr) {
    MS_LOG(ERROR) << "null pointer!";
    return RET_NULL_PTR;
  }

  auto bias_parameter_ptr = std::dynamic_pointer_cast<Parameter>(bias);
  auto bias_default_param = bias_parameter_ptr->default_param();
  auto bias_param = std::dynamic_pointer_cast<ParamValueLite>(bias_default_param);

  auto active_weight_quant_params = primitive_c->GetInputQuantParams();
  if (active_weight_quant_params.size() != 2) {
    MS_LOG(ERROR) << "unexpected active_weight_quant_params size: " << active_weight_quant_params.size();
    return RET_ERROR;
  }

  auto active_params = active_weight_quant_params[0];
  auto weight_params = active_weight_quant_params[1];

  vector<double> input_scales;
  vector<double> filter_scales;
  vector<double> bias_scales;
  size_t sizeX = active_params.size();
  for (size_t i = 0; i < sizeX; i++) {
    input_scales.emplace_back(active_params[i].scale);
  }
  size_t sizeY = weight_params.size();
  if (sizeX != sizeY) {
    if (sizeX > 1 && sizeY > 1) {
      MS_LOG(ERROR) << "input and filter's scale count cannot match!";
      return RET_ERROR;
    }
  }
  for (size_t i = 0; i < sizeY; i++) {
    filter_scales.emplace_back(weight_params[i].scale);
  }
  size_t size = std::max(sizeX, sizeY);
  for (size_t i = 0; i < size; i++) {
    auto scaleX = sizeX > 1 ? input_scales[i] : input_scales[0];
    auto scaleY = sizeY > 1 ? filter_scales[i] : filter_scales[0];
    bias_scales.push_back(scaleX * scaleY);
  }
  MS_ASSERT(!bias_scales.empty());
  size_t shape_size = bias_param->tensor_shape_size();

  // set bias quant param
  vector<schema::QuantParamT> quant_params;
  for (size_t i = 0; i < bias_scales.size(); i++) {
    schema::QuantParamT quant_param;
    quant_param.scale = bias_scales[i];
    quant_param.zeroPoint = 0;
    quant_param.inited = true;
    quant_params.emplace_back(quant_param);
  }
  // quant bias data
  int32_t *quant_datas = new (std::nothrow) int32_t[shape_size];
  if (quant_datas == nullptr) {
    MS_LOG(ERROR) << "null pointer dereferencing.";
    return RET_NULL_PTR;
  }
  float *raw_datas = static_cast<float *>(bias_param->tensor_addr());
  double bias_scale_tmp;
  constexpr int32_t quanted_bias_abs_limit = 0.5 * INT32_MAX;
  for (size_t i = 0; i < shape_size; i++) {
    if (bias_scales.size() == 1) {
      bias_scale_tmp = bias_scales[0];
    } else {
      bias_scale_tmp = bias_scales[i];
    }
    if (std::abs(raw_datas[i] / bias_scale_tmp) >= quanted_bias_abs_limit) {
      MS_LOG(DEBUG) << "quanted bias over flow, maybe the scale of weight: " << active_weight_quant_params[1][i].scale
                    << " is too small, need to update";
      // update filter scale and zp
      if (input_scales.size() == 1 && active_weight_quant_params[1].size() == shape_size) {
        double activate_scale = input_scales[0];
        double filter_scale = std::abs(raw_datas[i]) / (activate_scale * quanted_bias_abs_limit);
        active_weight_quant_params[1][i].scale = filter_scale;
        active_weight_quant_params[1][i].zeroPoint = 0;
        primitive_c->SetInputQuantParam(active_weight_quant_params);
        bias_scale_tmp = std::abs(raw_datas[i]) / quanted_bias_abs_limit;
        quant_params[i].scale = bias_scale_tmp;
        MS_LOG(DEBUG) << "new filter scale: " << filter_scale;
      } else {
        MS_LOG(WARNING) << "unexpected input_scales size: " << input_scales.size() << " weight_scales size: "
                        << active_weight_quant_params[1].size();
      }
    }
    auto quant_data = (int32_t)std::round(raw_datas[i] / bias_scale_tmp);
    quant_datas[i] = quant_data;
  }
  primitive_c->AddInputQuantParam(quant_params);
  auto ret = memcpy_s(bias_param->tensor_addr(), bias_param->tensor_size(), quant_datas, shape_size * sizeof(int32_t));
  if (ret != EOK) {
    MS_LOG(ERROR) << "memcpy_s failed.";
    delete[] quant_datas;
    return RET_ERROR;
  }
  delete[] quant_datas;
  // set dtype
  auto abstractBase = bias_parameter_ptr->abstract();
  if (abstractBase == nullptr) {
    MS_LOG(ERROR) << "Abstract of parameter is nullptr, " << bias_parameter_ptr->name();
    return RET_ERROR;
  }
  if (!utils::isa<abstract::AbstractTensorPtr>(abstractBase)) {
    MS_LOG(ERROR) << "Abstract of parameter should be anstract tensor, " << bias_parameter_ptr->name();
    return RET_ERROR;
  }
  auto abstractTensor = utils::cast<abstract::AbstractTensorPtr>(abstractBase);
  abstractTensor->element()->set_type(TypeIdToType(kNumberTypeInt32));
  return RET_OK;
}

STATUS PostTrainingQuantizer::QuantNode() {
  auto input_min_max = this->calibrator_->GetMinMax(this->calibrator_->GetInputDivergInfo());
  auto input_scale = this->calibrator_->GetScale(this->calibrator_->GetInputDivergInfo());
  auto input_zero_point = this->calibrator_->GetZeropoint(this->calibrator_->GetInputDivergInfo());

  auto output_min_max = this->calibrator_->GetMinMax(this->calibrator_->GetOutputDivergInfo());
  auto output_scale = this->calibrator_->GetScale(this->calibrator_->GetOutputDivergInfo());
  auto output_zeropoint = this->calibrator_->GetZeropoint(this->calibrator_->GetOutputDivergInfo());

  auto cnodes = funcGraph->GetOrderedCnodes();
  for (auto &cnode : cnodes) {
    auto op_name = cnode->fullname_with_scope();
    if (this->calibrator_->GetInputDivergInfo()->find(op_name) == this->calibrator_->GetInputDivergInfo()->end()) {
      MS_LOG(INFO) << op_name << " can not do quant";
      continue;
    }
    auto primitive_c = GetValueNode<std::shared_ptr<PrimitiveC>>(cnode->input(0));
    if (primitive_c == nullptr) {
      MS_LOG(ERROR) << "primitive_c is nullptr";
      continue;
    }
    if (input_scale.find(cnode) == input_scale.end()) {
      primitive_c->SetQuantType(schema::QuantType_QUANT_NONE);
      continue;
    }

    auto op_type = (schema::PrimitiveType)primitive_c->Type();
    MS_LOG(INFO) << "OpName: " << op_name;
    if (op_type != PrimitiveType_Conv2D && op_type != PrimitiveType_DepthwiseConv2D &&
        op_type != PrimitiveType_FullConnection) {
      for (size_t i = 1; i < cnode->inputs().size(); i++) {
        auto input_node = cnode->input(i);
        if (!input_node->isa<mindspore::CNode>()) {
          MS_LOG(DEBUG) << "node: " << op_name << " input " << i << " not a cnode";
          // get dtype
          auto abstractBase = input_node->abstract();
          if (abstractBase == nullptr) {
            MS_LOG(ERROR) << "Abstract of parameter is nullptr, " << input_node->fullname_with_scope();
            return RET_ERROR;
          }
          if (!utils::isa<abstract::AbstractTensorPtr>(abstractBase)) {
            MS_LOG(ERROR) << "Abstract of parameter should be anstract tensor, " << input_node->fullname_with_scope();
            return RET_ERROR;
          }
          auto abstractTensor = utils::cast<abstract::AbstractTensorPtr>(abstractBase);
          if (abstractTensor->element()->GetTypeTrack()->type_id() == kNumberTypeFloat32) {
            MS_LOG(DEBUG) << "this parameter do quant";
            DoWeightQuant(input_node, primitive_c, false);
          } else {
            MS_LOG(DEBUG) << "this parameter no need to do quant";
          }
          continue;
        }
        auto input_cnode = std::dynamic_pointer_cast<mindspore::CNode>(input_node);
        auto input_cnode_primitive_c = GetValueNode<std::shared_ptr<PrimitiveC>>(input_cnode->input(0));
        if (input_cnode_primitive_c == nullptr) {
          MS_LOG(DEBUG) << "input: " << i << " " << input_cnode->fullname_with_scope() << ": "
                        << " PrimitiveC is null";
          continue;
        }
        if (!input_cnode_primitive_c->GetOutputQuantParams().empty()) {
          for (auto &quant_param : input_cnode_primitive_c->GetOutputQuantParams()) {
            primitive_c->AddInputQuantParam(quant_param);
          }
        } else {
          // do input quant
          double scale = input_scale[cnode];
          int32_t zp = input_zero_point[cnode];
          DoQuantInput(scale, zp, &input_min_max[cnode], primitive_c);
        }
      }
    } else {
      // do input quant
      double scale = input_scale[cnode];
      int32_t convInputzeropoint = input_zero_point[cnode];
      DoQuantInput(scale, convInputzeropoint, &input_min_max[cnode], primitive_c);
      // do weight quant
      auto weight = cnode->input(2);
      bool perchannel = per_channel_;
      if (op_type == PrimitiveType_FullConnection) {
        perchannel = false;
      }
      DoWeightQuant(weight, primitive_c, perchannel);
      // do bias quant
      if (cnode->inputs().size() == 4) {
        auto bias = cnode->input(3);
        DoBiasQuant(bias, primitive_c);
      }
    }
    // do output quant
    double OutputScale = output_scale[cnode];
    int32_t OutputZeropoint = output_zeropoint[cnode];
    DoQuantOutput(OutputScale, OutputZeropoint, &output_min_max[cnode], primitive_c);
    primitive_c->SetQuantType(schema::QuantType_PostTraining);
  }
  return RET_OK;
}

STATUS PostTrainingQuantizer::UpdateDivergInverval() {
  this->calibrator_->UpdateDivergInverval(this->calibrator_->GetInputDivergInfo());
  this->calibrator_->UpdateDivergInverval(this->calibrator_->GetOutputDivergInfo());
  return RET_OK;
}

/**
 * Pre Process
 * 1. generate config param
 *   1.1 read config file
 *   1.2 parse txt
 * 2. collect image files
 *   2.1 parse image files to input tensor
 * 3. save quantied node
 **/
STATUS PostTrainingQuantizer::PreProcess() {
  if (this->calibrator_ == nullptr) {
    MS_LOG(ERROR) << "calibrator is null!";
    return RET_ERROR;
  }
  // 1. generate config param
  STATUS status = calibrator_->ReadConfig();
  if (status != RET_OK) {
    MS_LOG(ERROR) << "read proto text failed!";
    return status;
  }
  // 2. collect image files
  status = calibrator_->CollectImages();
  if (status != RET_OK) {
    MS_LOG(ERROR) << "collect images failed!";
    return status;
  }
  // 3. collect to be quantized operators
  // from user input
  QuantStrategy strategy(10);
  auto cnodes = funcGraph->GetOrderedCnodes();
  for (auto &cnode : cnodes) {
    AnfNodePtr anf = std::dynamic_pointer_cast<AnfNode>(cnode);
    if (strategy.CanOpPostQuantized(anf)) {
      MS_LOG(INFO) << "node: " << cnode->fullname_with_scope() << " will be quantized";
      calibrator_->AddQuantizedOp(cnode);
    }
  }
  return RET_OK;
}

STATUS PostTrainingQuantizer::CheckTensorVec(const std::string &node_name,
                                             const std::vector<mindspore::tensor::MSTensor *> &tensor_vec) const {
  if (tensor_vec.size() < 1) {
    MS_LOG(ERROR) << "node: " << node_name << " input tensors is 0";
    return RET_ERROR;
  }
  auto *tensor = tensor_vec[0];
  if (tensor->data_type() != kNumberTypeFloat32) {
    MS_LOG(DEBUG) << "node: " << node_name << " will not quantize"
                  << " tensor data_type: " << tensor->data_type();
    return RET_ERROR;
  }
  return RET_OK;
}

/**
 * 1. create input tensor
 * 2. insert callback to session
 * 3. run session
 **/
STATUS PostTrainingQuantizer::DoInference() {
  for (size_t i = 0; i < calibrator_->GetBatchNum(); i++) {
    // get input tensor
    vector<mindspore::tensor::MSTensor *> inputs = session_->GetInputs();
    if (inputs.size() > 1) {
      MS_LOG(ERROR) << "model's input tensor size: " << inputs.size() << " >1";
      return RET_ERROR;
    }
    STATUS status = calibrator_->GenerateInputData(i, inputs.front());
    if (status != RET_OK) {
      MS_LOG(ERROR) << "generate input data from images failed!";
      return RET_ERROR;
    }
    mindspore::session::KernelCallBack beforeCallBack =
      [&](const std::vector<mindspore::tensor::MSTensor *> &beforeInputs,
          const std::vector<mindspore::tensor::MSTensor *> &beforeOutputs,
          const mindspore::session::CallBackParam &callParam) -> bool {
      if (PostTrainingQuantizer::CheckTensorVec(callParam.name_callback_param, beforeInputs) != RET_OK) {
        return false;
      }
      auto tensor = beforeInputs[0];
      const float *tData = static_cast<const float *>(tensor->MutableData());
      size_t elem_count = tensor->ElementsNum();
      vector<float> data(tData, tData + elem_count);
      this->calibrator_->RecordMaxValue(callParam.name_callback_param, data, this->calibrator_->GetInputDivergInfo());
      return true;
    };
    // func
    mindspore::session::KernelCallBack afterCallBack = [&](
                                                         const std::vector<mindspore::tensor::MSTensor *> &afterInputs,
                                                         const std::vector<mindspore::tensor::MSTensor *> &afterOutputs,
                                                         const mindspore::session::CallBackParam &callParam) -> bool {
      if (PostTrainingQuantizer::CheckTensorVec(callParam.name_callback_param, afterOutputs) != RET_OK) {
        return false;
      }
      auto tensor = afterOutputs[0];
      const float *tensor_data = static_cast<const float *>(tensor->MutableData());
      size_t elem_count = tensor->ElementsNum();
      vector<float> data(tensor_data, tensor_data + elem_count);
      this->calibrator_->RecordMaxValue(callParam.name_callback_param, data, this->calibrator_->GetOutputDivergInfo());
      return true;
    };
    status = session_->RunGraph(beforeCallBack, afterCallBack);
    if (status != RET_OK) {
      MS_LOG(ERROR) << "run model failed!";
      return RET_ERROR;
    }
  }
  return RET_OK;
}

STATUS PostTrainingQuantizer::CollectDataFrequency() {
  for (size_t i = 0; i < calibrator_->GetBatchNum(); i++) {
    // get input tensor
    vector<mindspore::tensor::MSTensor *> inputs = session_->GetInputs();
    if (inputs.size() > 1) {
      MS_LOG(ERROR) << "model's input tensor size: " << inputs.size() << " > 1";
      return RET_ERROR;
    }
    STATUS status = calibrator_->GenerateInputData(i, inputs.front());
    if (status != RET_OK) {
      MS_LOG(ERROR) << "generate input data from images failed!";
      return RET_ERROR;
    }

    mindspore::session::KernelCallBack beforeCallBack =
      [&](const std::vector<mindspore::tensor::MSTensor *> &beforeInputs,
          const std::vector<mindspore::tensor::MSTensor *> &beforeOutputs,
          const mindspore::session::CallBackParam &callParam) {
        if (PostTrainingQuantizer::CheckTensorVec(callParam.name_callback_param, beforeInputs) != RET_OK) {
          return false;
        }
        auto tensor = beforeInputs[0];
        const float *tensor_data = static_cast<const float *>(tensor->MutableData());
        size_t shape_size = tensor->ElementsNum();
        vector<float> data(tensor_data, tensor_data + shape_size);
        this->calibrator_->UpdateDataFrequency(callParam.name_callback_param, data,
                                               this->calibrator_->GetInputDivergInfo());
        return true;
      };

    mindspore::session::KernelCallBack afterCallBack =
      [&](const std::vector<mindspore::tensor::MSTensor *> &after_inputs,
          const std::vector<mindspore::tensor::MSTensor *> &after_outputs,
          const mindspore::session::CallBackParam &call_param) {
        if (PostTrainingQuantizer::CheckTensorVec(call_param.name_callback_param, after_outputs) != RET_OK) {
          return false;
        }
        auto tensor = after_outputs[0];
        const float *tenosr_data = static_cast<const float *>(tensor->MutableData());
        size_t shape_size = tensor->ElementsNum();
        vector<float> data(tenosr_data, tenosr_data + shape_size);
        this->calibrator_->UpdateDataFrequency(call_param.name_callback_param, data,
                                               this->calibrator_->GetOutputDivergInfo());
        return true;
      };
    status = session_->RunGraph(beforeCallBack, afterCallBack);
    if (status != RET_OK) {
      MS_LOG(ERROR) << "run model failed!";
      return RET_ERROR;
    }
  }

  return RET_OK;
}

STATUS PostTrainingQuantizer::ComputeThreshold() { return this->calibrator_->ComputeThreshold(); }

STATUS PostTrainingQuantizer::DoQuantize(FuncGraphPtr funcGraph) {
  MS_LOG(INFO) << "start to parse config file";
  STATUS status = PreProcess();
  if (status != RET_OK) {
    MS_LOG(ERROR) << "do pre process failed!";
    return status;
  }

  // anf -- fb
  auto meta_graph = Export(funcGraph, true);
  if (meta_graph == nullptr) {
    MS_LOG(ERROR) << "Export to meta_graph return nullptr";
    return RET_ERROR;
  }

  // transform
  GraphDefTransform transform;
  transform.SetGraphDef(meta_graph);
  flags.quantType = schema::QuantType_QUANT_NONE;
  status = transform.Transform(flags);
  if (status != RET_OK) {
    MS_LOG(ERROR) << "FBTransform model failed " << status;
    return RET_ERROR;
  }
  MS_LOG(INFO) << "start create session";
  flatbuffers::FlatBufferBuilder builder(1024);
  auto offset = schema::MetaGraph::Pack(builder, meta_graph);
  builder.Finish(offset);
  size_t size = builder.GetSize();
  auto *content = reinterpret_cast<const char *>(builder.GetBufferPointer());
  if (content == nullptr) {
    MS_LOG(ERROR) << "GetBufferPointer nullptr";
    return RET_ERROR;
  }
  auto model = lite::Model::Import(content, size);

  Context ctx;
  ctx.device_type_ = DT_CPU;
  ctx.thread_num_ = calibrator_->GetThreadNum();
  ctx.cpu_bind_mode_ = MID_CPU;

  session_ = dynamic_cast<mindspore::lite::LiteSession *>(session::LiteSession::CreateSession(&ctx));
  if (session_ == nullptr) {
    MS_LOG(ERROR) << "create session failed!";
    return RET_ERROR;
  }

  auto ret = session_->CompileGraph(model);
  if (ret != lite::RET_OK) {
    MS_LOG(ERROR) << "compile graph error";
    return RET_ERROR;
  }

  MS_LOG(INFO) << "start to update divergence's max value";
  status = DoInference();
  if (status != RET_OK) {
    return status;
  }
  MS_LOG(INFO) << "start to update divergence's interval";
  status = UpdateDivergInverval();
  if (status != RET_OK) {
    return status;
  }
  MS_LOG(INFO) << "start to collect data's distribution";
  status = CollectDataFrequency();
  if (status != RET_OK) {
    return status;
  }
  MS_LOG(INFO) << "compute the best threshold";
  status = ComputeThreshold();
  if (status != RET_OK) {
    return status;
  }
  MS_LOG(INFO) << "start to generate quant param and quantize tensor's data";
  status = QuantNode();
  if (status != RET_OK) {
    return status;
  }
  return RET_OK;
}
}  // namespace quant
}  // namespace lite
}  // namespace mindspore
