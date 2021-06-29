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

#include "frontend/parallel/ops_info/conv2d_info.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "frontend/parallel/device_matrix.h"
#include "frontend/parallel/strategy.h"
#include "frontend/parallel/tensor_layout/tensor_redistribution.h"
#include "pipeline/jit/resource.h"

namespace mindspore {
namespace parallel {
Status Conv2DInfo::GetAttrsBase() {
  // out_channel
  out_channel_ = GetIntAttr(OUT_CHANNEL);
  if (out_channel_ <= 0) {
    MS_LOG(ERROR) << name_ << ": The attr of out_channel is invalid";
    return FAILED;
  }

  // kernel_size
  auto kernel_size_iter = attrs_.find(KERNEL_SIZE);
  if (kernel_size_iter == attrs_.end()) {
    MS_LOG(ERROR) << name_ << ": Can not find the attribution of " << KERNEL_SIZE;
    return FAILED;
  }

  MS_EXCEPTION_IF_NULL(kernel_size_iter->second);
  if (kernel_size_iter->second->isa<Int64Imm>()) {
    int64_t kernel_size = kernel_size_iter->second->cast<Int64ImmPtr>()->value();
    kernel_size_ = {kernel_size, kernel_size};
  } else if (kernel_size_iter->second->isa<ValueTuple>() || kernel_size_iter->second->isa<ValueList>()) {
    kernel_size_ = GetValue<std::vector<int64_t>>(kernel_size_iter->second);
    if (kernel_size_.size() != 2) {
      MS_LOG(ERROR) << name_ << ": The size of kernel_size'tuple must be 2, but got " << kernel_size_.size();
      return FAILED;
    }
  } else {
    MS_LOG(ERROR) << name_ << ": The kernel_size must be int or tuple";
    return FAILED;
  }

  // mode
  mode_ = GetIntAttr(MODE);
  if (mode_ != 1) {
    MS_LOG(ERROR) << name_ << ": The mode must be 1, but got " << mode_;
    return FAILED;
  }

  // pad_mode
  pad_mode_ = GetIntAttr(PAD_MODE);
  if (pad_mode_ < 0 || pad_mode_ > 2) {
    MS_LOG(ERROR) << name_ << ": The pad_mode must be in the range of [0, 2], but got " << pad_mode_;
    return FAILED;
  }

  // pad_list
  pad_list_ = GetTupleIntAttr(PAD_LIST);
  if (pad_list_.size() != 4) {
    MS_LOG(ERROR) << name_ << ": The size of pad_list must be 4, but got " << pad_list_.size();
    return FAILED;
  }

  // stride
  stride_ = GetTupleIntAttr(STRIDE);
  if (stride_.size() != 4) {
    MS_LOG(ERROR) << name_ << ": The size of stride must be 4, but got " << stride_.size();
    return FAILED;
  }

  if (stride_[0] != 1 || stride_[1] != 1) {
    MS_LOG(ERROR) << name_ << ": The first two elements of stride must be 1, but got (" << stride_[0] << ", "
                  << stride_[1] << ")";
    return FAILED;
  }

  // dilation
  dilation_ = GetTupleIntAttr(DILATION);
  if (dilation_.size() != 4) {
    MS_LOG(ERROR) << name_ << ": The size of dilation must be 4, but got " << dilation_.size();
    return FAILED;
  }

  // group
  group_ = GetIntAttr(GROUP);
  if (group_ != 1) {
    MS_LOG(ERROR) << name_ << ": The group must be 1, but got " << group_;
    return FAILED;
  }

  // format
  format_ = GetStringAttr(FORMAT);
  if (format_ != NCHW) {
    MS_LOG(ERROR) << name_ << ": The format must be 'NCHW', but got " << format_;
    return FAILED;
  }

  MS_LOG(INFO) << name_ << ": The out channel is " << out_channel_ << ", kernel size is " << kernel_size_
               << ", mode is " << mode_ << ", pad mode is " << pad_mode_ << ", pad list is " << pad_list_
               << ", stride is " << stride_ << ", dilation is " << dilation_ << ", group is " << group_
               << ", format is " << format_;

  return SUCCESS;
}

Status Conv2DInfo::GetAttrs() { return GetAttrsBase(); }

Status Conv2DInfo::CheckHWStrategy(int64_t h_strategy, int64_t w_strategy) {
  if (pad_mode_ == 0) {  // 'pad' mode
    MS_LOG(ERROR) << name_ << ": The 'pad' mode do not support to split H or W";
    return FAILED;
  }

  if (pad_mode_ == 1) {  // 'same' mode
    if ((kernel_size_[0] > stride_[2] || kernel_size_[1] > stride_[3]) && h_strategy > 1) {
      MS_LOG(ERROR) << name_ << ": The 'same' mode do not support to split H when kernel_size > stride";
      return FAILED;
    }

    if (kernel_size_[0] <= stride_[2] || kernel_size_[1] <= stride_[3]) {
      int64_t h_slice_shape = inputs_shape_[0][2] / h_strategy;
      int64_t w_slice_shape = inputs_shape_[0][3] / w_strategy;
      if (h_slice_shape % stride_[2] != 0 || w_slice_shape % stride_[3] != 0) {
        MS_LOG(ERROR) << name_
                      << ": The 'same' mode do not support to split H or W when kernel_size <= stride but slice shape "
                         "is not divisible by stride ";
        return FAILED;
      }
    }
  }

  if (pad_mode_ == 2) {  // 'valid' mode
    if ((kernel_size_[0] > stride_[2] && h_strategy > 1) || (kernel_size_[1] > stride_[3] && w_strategy > 1)) {
      MS_LOG(ERROR) << name_ << ": The 'valid' mode do not support to split H or W when kernel_size > stride";
      return FAILED;
    }

    if (kernel_size_[0] <= stride_[2]) {
      int64_t h_slice_shape = inputs_shape_[0][2] / h_strategy;
      if (h_slice_shape % stride_[2] != 0) {
        MS_LOG(ERROR) << name_
                      << ": The 'valid' mode do not support to split H when kernel_size <= stride but slice shape is "
                         "not divisible by stride ";
        return FAILED;
      }
    }

    if (kernel_size_[1] <= stride_[3]) {
      int64_t w_slice_shape = inputs_shape_[0][3] / w_strategy;
      if (w_slice_shape % stride_[3] != 0) {
        MS_LOG(ERROR) << name_
                      << ": The 'valid' mode do not support to split W when kernel_size <= stride but slice shape is "
                         "not divisible by stride ";
        return FAILED;
      }
    }
  }

  return SUCCESS;
}

Status Conv2DInfo::CheckStrategyBase(const StrategyPtr &strategy) {
  MS_EXCEPTION_IF_NULL(strategy);
  if (CheckStrategyValue(strategy, inputs_shape_) != SUCCESS) {
    MS_LOG(ERROR) << name_ << ": Invalid strategy";
    return FAILED;
  }

  std::vector<Dimensions> stra = strategy->GetInputDim();
  if (stra.size() != 2) {
    MS_LOG(ERROR) << name_ << ": The size of strategy must be 2, but got " << stra.size();
    return FAILED;
  }

  Dimensions input_strategy = stra[0];
  Dimensions weight_strategy = stra[1];
  if (input_strategy.size() != 4 || weight_strategy.size() != 4) {
    MS_LOG(ERROR) << name_
                  << ": The size of input strategy or weight strategy must be 4, but the size of input strategy is "
                  << input_strategy.size() << ", the size of weight strategy is " << weight_strategy.size();
    return FAILED;
  }

  if (weight_strategy[2] != 1 || weight_strategy[3] != 1) {
    MS_LOG(ERROR) << name_ << ": The kernel size can not be split, but the strategy for kernel size is ("
                  << weight_strategy[2] << ", " << weight_strategy[3] << ")";
    return FAILED;
  }

  if (weight_strategy[0] > 1) {
    out_channel_shard_ = true;
    new_out_channel_ = out_channel_ / weight_strategy[1];
  } else {
    out_channel_shard_ = false;
  }

  return SUCCESS;
}

Status Conv2DInfo::CheckStrategy(const StrategyPtr &strategy) {
  need_exchange_overlap_ = false;
  if (CheckStrategyBase(strategy) != SUCCESS) {
    return FAILED;
  }

  std::vector<Dimensions> stra = strategy->GetInputDim();
  Dimensions input_strategy = stra[0];
  Dimensions weight_strategy = stra[1];
  if (input_strategy[1] != weight_strategy[1]) {
    MS_LOG(ERROR) << name_ << ": The shard num of c-in for input strategy is " << input_strategy[1]
                  << ", but the shard num of c-in for weight strategy is " << weight_strategy[1];
    return FAILED;
  }

  if (input_strategy[2] != 1 || input_strategy[3] != 1) {
    if (CheckHWStrategy(input_strategy[2], input_strategy[3]) != SUCCESS) {
      return FAILED;
    }
  }

  // kernel size larger than stride and the w dimension is split, need to exchange overlap
  if ((kernel_size_[1] > stride_[3]) && (input_strategy[3] > 1)) {
    need_exchange_overlap_ = true;
  }

  return SUCCESS;
}

Status Conv2DInfo::InferDevMatrixShape() {
  // the strategy is ((n, i, h, w), (o, i, 1, 1))
  // the dev matrix is (n, i, h, w, o)
  MS_EXCEPTION_IF_NULL(strategy_);
  std::vector<Dimensions> stra = strategy_->GetInputDim();
  if (stra.size() != 2) {
    MS_LOG(ERROR) << name_ << ": The size of strategy must be 2, but got " << stra.size();
    return FAILED;
  }

  dev_matrix_shape_ = stra[0];
  dev_matrix_shape_.push_back(stra[1][0]);
  w_dimension_shard_num_ = stra[0][3];
  input_slice_shape_ = GetSliceShape(inputs_shape_[0], stra[0]);
  return SUCCESS;
}

Status Conv2DInfo::InferRankBias() {
  // the origin dev_matrix is [n, i, h, w, o]
  // if repeated calculation and repeated num in the left of dev matrix, the dev_matrix is [repeated_num, n, i, h, w, o]
  // if repeated calculation and repeated num in the right of dev matrix, the dev_matrix is [n, i, h, w, o,
  // repeated_num] the rank_bias_ is the position of the current rank in the w dimension of the dev_matrix(have not
  // split h dimension)
  if (!need_exchange_overlap_) {
    MS_LOG(INFO) << name_ << ": No need to infer rank bias";
    return SUCCESS;
  }

  uint64_t w_index_in_dev_matrix = 3;
  if (repeated_calc_num_ > 1 && !repeated_num_in_dev_matrix_right_) {
    w_index_in_dev_matrix += 1;
  }

  CheckGlobalDeviceManager();
  int64_t rank = g_device_manager->global_rank();
  DeviceMatrix dev_matrix(rank, stage_device_list_, dev_matrix_shape_);
  RankList group_devices;
  if (dev_matrix.GetDevicesAlongDim(w_index_in_dev_matrix, &group_devices) != SUCCESS) {
    return FAILED;
  }

  if (group_devices.size() <= 1) {
    MS_LOG(INFO) << name_ << ": The devices' size of w dimension is " << group_devices.size()
                 << ", no need to infer rank bias";
    return SUCCESS;
  }

  if (group_devices.size() != LongToSize(w_dimension_shard_num_)) {
    MS_LOG(ERROR) << name_ << ": The devices' size of w dimension is " << group_devices.size()
                  << ", but the shard num of w dimension is " << w_dimension_shard_num_;
    return FAILED;
  }

  std::vector<int64_t>::iterator it = std::find(group_devices.begin(), group_devices.end(), rank);
  if (it == group_devices.end()) {
    MS_LOG(ERROR) << name_ << ": Can not find the current rank in device list of w dimension, the current rank is "
                  << rank << ", the device list is " << group_devices;
    return FAILED;
  }

  rank_bias_ = std::distance(group_devices.begin(), it);
  if (it == group_devices.begin()) {
    left_rank_bias_ = -1;
    right_rank_bias_ = rank_bias_ + 1;
  } else if (it == group_devices.end() - 1) {
    left_rank_bias_ = rank_bias_ - 1;
    right_rank_bias_ = -1;
  } else {
    left_rank_bias_ = rank_bias_ - 1;
    right_rank_bias_ = rank_bias_ + 1;
  }
  MS_LOG(INFO) << name_ << ": The current rank is " << rank << ", the device list of w dimension is " << group_devices
               << ", the rank bias is " << rank_bias_ << ", the left rank bias is " << left_rank_bias_
               << ", the right rank bias is " << right_rank_bias_;
  return SUCCESS;
}

int64_t Conv2DInfo::ComputeOverlapLeftSizeByRankBias(int64_t rank_bias) {
  int64_t left_pad = pad_list_[2];
  int64_t w_dimension_input_shape = inputs_shape_[0][3];
  int64_t w_dimension_output_shape = outputs_shape_[0][3];
  int64_t w_stride = stride_[3];

  return left_pad +
         (w_dimension_input_shape - w_dimension_output_shape * w_stride) * rank_bias / w_dimension_shard_num_;
}

int64_t Conv2DInfo::ComputeOverlapRightSizeByRankBias(int64_t rank_bias) {
  int64_t left_pad = pad_list_[2];
  int64_t w_dimension_input_shape = inputs_shape_[0][3];
  int64_t w_dimension_output_shape = outputs_shape_[0][3];
  int64_t w_kernel_size = kernel_size_[1];
  int64_t w_stride = stride_[3];

  return (rank_bias + 1) * (w_dimension_output_shape * w_stride - w_dimension_input_shape) / w_dimension_shard_num_ +
         w_kernel_size - w_stride - left_pad;
}

Status Conv2DInfo::InferOverlapSize() {
  if (!need_exchange_overlap_) {
    MS_LOG(INFO) << name_ << ": No need to infer overlap size";
    return SUCCESS;
  }

  overlap_left_size_ = ComputeOverlapLeftSizeByRankBias(rank_bias_);
  overlap_right_size_ = ComputeOverlapRightSizeByRankBias(rank_bias_);

  if (rank_bias_ == 0) {  // it has not left rank
    left_rank_overlap_left_size_ = 0;
    left_rank_overlap_right_size_ = 0;
    right_rank_overlap_left_size_ = ComputeOverlapLeftSizeByRankBias(right_rank_bias_);
    right_rank_overlap_right_size_ = ComputeOverlapRightSizeByRankBias(right_rank_bias_);
  } else if (rank_bias_ == w_dimension_shard_num_ - 1) {  // it has not right rank
    left_rank_overlap_left_size_ = ComputeOverlapLeftSizeByRankBias(left_rank_bias_);
    left_rank_overlap_right_size_ = ComputeOverlapRightSizeByRankBias(left_rank_bias_);
    right_rank_overlap_left_size_ = 0;
    right_rank_overlap_right_size_ = 0;
  } else {  // it has left rank and right rank
    left_rank_overlap_left_size_ = ComputeOverlapLeftSizeByRankBias(left_rank_bias_);
    left_rank_overlap_right_size_ = ComputeOverlapRightSizeByRankBias(left_rank_bias_);
    right_rank_overlap_left_size_ = ComputeOverlapLeftSizeByRankBias(right_rank_bias_);
    right_rank_overlap_right_size_ = ComputeOverlapRightSizeByRankBias(right_rank_bias_);
  }

  MS_LOG(INFO) << name_ << ": the left overlap size of current rank is " << overlap_left_size_
               << ", the right overlap size of current rank is " << overlap_right_size_
               << ", the left overlap size of left rank is " << left_rank_overlap_left_size_
               << ", the right overlap size of left rank is " << left_rank_overlap_right_size_
               << ", the left overlap size of right rank is " << right_rank_overlap_left_size_
               << ", the right overlap size of right rank is " << right_rank_overlap_right_size_;
  return SUCCESS;
}

Status Conv2DInfo::InferTensorMap() {
  // input_strategy: ((n, i, h, w), (o, i, 1, 1))
  // output_strategy: ((n, o, h, w),)
  // dev_matrix: (n, i, h, w, o)
  TensorMap input_tensor_map = {4, 3, 2, 1};
  TensorMap weight_tensor_map = {0, 3, -1, -1};
  TensorMap output_tensor_map = {4, 0, 2, 1};

  (void)inputs_tensor_map_.emplace_back(std::move(input_tensor_map));
  (void)inputs_tensor_map_.emplace_back(std::move(weight_tensor_map));
  (void)outputs_tensor_map_.emplace_back(std::move(output_tensor_map));
  return SUCCESS;
}

// Conv2d: dev_matrix is (n, i, h, w, o), if in channel is split, it need to insert all reduce
// Conv2DBackpropInputInfo: dev_matrix is (n, o, h, w, i), if out channel is split, it need to insert all reduce
Status Conv2DInfo::InferForwardCommunication() {
  forward_op_.clear();
  size_t relevant_dim_index = IN_CHANNEL_INDEX;
  if (repeated_calc_num_ > 1 && !repeated_num_in_dev_matrix_right_) {
    // if repeated calculation and repeated num in the left of dev matrix, the index of relevant dimension should add 1
    relevant_dim_index += 1;
  }

  if (dev_matrix_shape_[relevant_dim_index] == MIN_SLICE_NUM) {
    MS_LOG(INFO) << name_ << ": Forward all reduce is not required";
    return SUCCESS;
  }

  std::vector<Group> group_list;
  if (CreateGroupByDim(relevant_dim_index, &group_list) != SUCCESS) {
    MS_LOG(ERROR) << name_ << ": Create group failed";
    return FAILED;
  }

  if (group_list.empty()) {
    MS_LOG(INFO) << name_ << ": Forward all reduce is not required";
    return SUCCESS;
  }

  Operator op = CreateAllReduceOp(REDUCE_OP_SUM, group_list[0].name());
  forward_op_.push_back(op);
  MS_LOG(INFO) << name_ << ": The group name of forward all reduce is " << group_list[0].name();

  return SUCCESS;
}

void Conv2DInfo::InferNewOperatorAttrs() {
  // send/recv flag and new_pad_list
  new_pad_list_ = pad_list_;
  if (rank_bias_ == 0) {  // the first rank
    left_need_send_ = false;
    left_need_recv_ = false;
    right_need_send_ = (right_rank_overlap_left_size_ > 0);
    right_need_recv_ = (overlap_right_size_ > 0);
    new_pad_list_[3] = 0;                                 // no need the right pad
  } else if (rank_bias_ == w_dimension_shard_num_ - 1) {  // the last rank
    left_need_send_ = (left_rank_overlap_right_size_ > 0);
    left_need_recv_ = (overlap_left_size_ > 0);
    right_need_send_ = false;
    right_need_recv_ = false;
    new_pad_list_[2] = 0;  // no need the left pad
  } else {                 // the middle rank
    left_need_send_ = (left_rank_overlap_right_size_ > 0);
    left_need_recv_ = (overlap_left_size_ > 0);
    right_need_send_ = (right_rank_overlap_left_size_ > 0);
    right_need_recv_ = (overlap_right_size_ > 0);
    new_pad_list_[2] = 0;  // no need the left pad
    new_pad_list_[3] = 0;  // no need the right pad
  }
  MS_LOG(INFO) << name_ << ": The left need send is " << left_need_send_ << ", the left need recv is "
               << left_need_recv_ << ", the right need send is " << right_need_send_ << ", the right need recv is "
               << right_need_recv_ << ", the new pad list is " << new_pad_list_;

  // the exchange rank ids
  if (left_need_send_ || left_need_recv_) {
    exchange_rank_ids_.push_back(left_rank_bias_);
  }

  if (right_need_send_ || right_need_recv_) {
    exchange_rank_ids_.push_back(right_rank_bias_);
  }
  MS_LOG(INFO) << name_ << ": The exchange rank ids is " << exchange_rank_ids_;

  // the recv reshapes
  if (left_need_recv_) {
    Shape left_recv_shape = input_slice_shape_;
    left_recv_shape[3] = overlap_left_size_;
    recv_shapes_.push_back(left_recv_shape);
  }

  if (right_need_recv_) {
    Shape right_recv_shape = input_slice_shape_;
    right_recv_shape[3] = overlap_right_size_;
    recv_shapes_.push_back(right_recv_shape);
  }
  MS_LOG(INFO) << name_ << ": the recv shapes is " << recv_shapes_;

  // the begin, end and strides of StridedSlice
  if (left_need_send_) {
    left_strided_slice_begin_ = {0, 0, 0, 0};
    left_strided_slice_end_ = input_slice_shape_;
    left_strided_slice_end_[3] = left_rank_overlap_right_size_;
    left_strided_slice_strides_ = {1, 1, 1, 1};
    MS_LOG(INFO) << name_ << ": The left strided slice begin is " << left_strided_slice_begin_ << ", end is "
                 << left_strided_slice_end_;
  }

  if (right_need_send_) {
    right_strided_slice_begin_ = {0, 0, 0, 0};
    right_strided_slice_begin_[3] = input_slice_shape_[3] - right_rank_overlap_left_size_;
    right_strided_slice_end_ = input_slice_shape_;
    right_strided_slice_strides_ = {1, 1, 1, 1};
    MS_LOG(INFO) << name_ << ": The right strided slice begin is " << right_strided_slice_begin_ << ", end is "
                 << right_strided_slice_end_;
  }
}

ReplaceGraphPtr Conv2DInfo::replace_graph(const CNodePtr &cnode) {
  if (!need_exchange_overlap_) {
    if (!out_channel_shard_) {
      return nullptr;
    }
    auto prim = GetValueNode<PrimitivePtr>(cnode->input(0));
    prim->set_attr(OUT_CHANNEL, MakeValue(new_out_channel_));
    return nullptr;
  }

  if (InferRankBias() != SUCCESS) {
    return nullptr;
  }

  if (InferOverlapSize() != SUCCESS) {
    return nullptr;
  }

  InferNewOperatorAttrs();

  return nullptr;
}

void Conv2DInfo::ReComputeBatchSplitFlagList() {
  split_flag_list_[0] = true;
  split_flag_list_[1] = false;
}

Status Conv2DInfo::SetCostUnderStrategy(const StrategyPtr &strategy) { return SetCostUnderStrategyBase(strategy); }

std::vector<StrategyPtr> Conv2DInfo::GenerateOpStrategies(int64_t stage_id) {
  Strategys strategy = {{stage_device_size_, 1, 1, 1}, {1, 1, 1, 1}};
  StrategyPtr sp = std::make_shared<Strategy>(stage_id, strategy);
  std::vector<StrategyPtr> sp_vector;
  sp_vector.push_back(sp);
  return sp_vector;
}

Status Conv2DInfo::Init(const StrategyPtr &strategy) {
  if (InitWithAutoRepeatCalc(strategy) != SUCCESS) {
    MS_LOG(ERROR) << name_ << ": Init failed.";
    return FAILED;
  }
  MS_LOG(INFO) << name_ << ": Init success.";
  return SUCCESS;
}

Status Conv2DInfo::InitForCostModel(const StrategyPtr &strategy) {
  if (InitForCostModelWithAutoRepeatCalc(strategy) != SUCCESS) {
    MS_LOG(ERROR) << name_ << ": Init for cost model failed.";
    return FAILED;
  }

  MS_LOG(INFO) << name_ << ": Init for cost model success.";
  return SUCCESS;
}

Status Conv2DBackpropInputInfo::GetOutShape() {
  if (input_value_.size() != 3) {
    MS_LOG(ERROR) << name_ << ": The size of input value must be 3, but got " << input_value_.size();
    return FAILED;
  }

  if (input_value_[2] == nullptr) {
    MS_LOG(ERROR) << name_ << ": The input_value_[2] is nullptr";
    return FAILED;
  }

  std::vector<ValuePtr> elements;
  auto value_tuple = input_value_[2]->cast<ValueTuplePtr>();
  if (value_tuple == nullptr) {
    MS_LOG(ERROR) << name_ << ": Input_value_[2] must be ValueTuplePtr.";
    return FAILED;
  }
  elements = value_tuple->value();
  if (elements.size() != 4) {
    MS_LOG(ERROR) << name_ << ": Elements size must be 4, but got " << elements.size();
    return FAILED;
  }

  for (auto &element : elements) {
    MS_EXCEPTION_IF_NULL(element);
    if (element->isa<Int64Imm>()) {
      int64_t axis = element->cast<Int64ImmPtr>()->value();
      out_shape_.push_back(axis);
    } else {
      MS_LOG(ERROR) << name_ << ": The value of shape must be int";
      return FAILED;
    }
  }

  return SUCCESS;
}

Status Conv2DBackpropInputInfo::GetAttrs() {
  if (GetAttrsBase() != SUCCESS) {
    return FAILED;
  }

  return GetOutShape();
}

Status Conv2DBackpropInputInfo::CheckStrategy(const StrategyPtr &strategy) {
  if (CheckStrategyBase(strategy) != SUCCESS) {
    return FAILED;
  }

  std::vector<Dimensions> stra = strategy->GetInputDim();
  Dimensions input_strategy = stra[0];
  Dimensions weight_strategy = stra[1];
  if (input_strategy[1] != weight_strategy[0]) {
    MS_LOG(ERROR) << name_ << ": The shard num of c-out for input strategy is " << input_strategy[1]
                  << ", but the shard num of c-out for weight strategy is " << weight_strategy[0];
    return FAILED;
  }

  if (input_strategy[2] != 1 || input_strategy[3] != 1) {
    if (CheckHWStrategy(input_strategy[2], input_strategy[3]) != SUCCESS) {
      return FAILED;
    }
  }

  return SUCCESS;
}

Status Conv2DBackpropInputInfo::CheckHWStrategy(int64_t h_strategy, int64_t w_strategy) { return SUCCESS; }

Status Conv2DBackpropInputInfo::InferDevMatrixShape() {
  // the strategy is ((n, o, h, w), (o, i, 1, 1))
  // the dev matrix is (n, o, h, w, i)
  MS_EXCEPTION_IF_NULL(strategy_);
  std::vector<Dimensions> stra = strategy_->GetInputDim();
  if (stra.size() != 2) {
    MS_LOG(ERROR) << name_ << ": The size of strategy must be 2, but got " << stra.size();
    return FAILED;
  }

  dev_matrix_shape_ = stra[0];
  dev_matrix_shape_.push_back(stra[1][1]);

  Shape out_strategy = stra[0];
  out_strategy[1] = stra[1][1];

  out_slice_shape_ = out_shape_;
  if (out_shape_.size() != out_strategy.size()) {
    MS_LOG(ERROR) << name_ << ": The size of out shape is " << out_shape_.size()
                  << ", but the size of output strategy is " << out_strategy.size();
    return FAILED;
  }

  for (size_t i = 0; i < out_slice_shape_.size(); ++i) {
    if (out_slice_shape_[i] % out_strategy[i] != 0) {
      MS_LOG(ERROR) << name_ << ": The output can not be split by strategy. The shape of output is " << out_slice_shape_
                    << ", but the strategy of output is " << out_strategy;
      return FAILED;
    }
    out_slice_shape_[i] = out_slice_shape_[i] / out_strategy[i];
  }

  MS_LOG(INFO) << name_ << ": The output slice shape is " << out_slice_shape_;
  return SUCCESS;
}

Status Conv2DBackpropInputInfo::InferTensorMap() {
  // input_strategy: ((n, o, h, w), (o, i, 1, 1))
  // output_strategy: ((n, i, h, w),)
  // dev_matrix: (n, o, h, w, i)
  TensorMap input_tensor_map = {4, 3, 2, 1};
  TensorMap weight_tensor_map = {3, 0, -1, -1};
  TensorMap output_tensor_map = {4, 0, 2, 1};

  (void)inputs_tensor_map_.emplace_back(std::move(input_tensor_map));
  (void)inputs_tensor_map_.emplace_back(std::move(weight_tensor_map));
  (void)outputs_tensor_map_.emplace_back(std::move(output_tensor_map));
  return SUCCESS;
}

Status Conv2DBackpropInputInfo::InferMirrorOps() {
  mirror_ops_.clear();
  if (inputs_shape_.empty()) {
    MS_LOG(INFO) << name_ << ": The inputs size is empty";
    return SUCCESS;
  }

  if (inputs_tensor_map_.size() != inputs_shape_.size()) {
    MS_LOG(ERROR) << name_ << ": The size of inputs tensor map is not equal to the size of inputs shape";
    return FAILED;
  }

  bool group_is_empty = true;
  for (size_t i = 0; i < inputs_tensor_map_.size(); ++i) {
    std::vector<Group> group;
    if (CreateGroupByTensorMap(inputs_tensor_map_[i], &group) != SUCCESS) {
      MS_LOG(ERROR) << name_ << ": Create group failed, the input index is " << i;
      mirror_ops_.clear();
      return FAILED;
    }

    OperatorVector mirror_op;
    if (group.empty()) {
      MS_LOG(INFO) << name_ << ": The mirror group is empty, the input index is " << i;
      mirror_ops_.push_back(mirror_op);
      continue;
    }

    group_is_empty = false;
    mirror_op = CreateMirrorOps(group[0].name(), group[0].GetDevNum());
    mirror_ops_.push_back(mirror_op);
  }

  if (group_is_empty) {
    mirror_ops_.clear();
    MS_LOG(INFO) << name_ << ": No need to insert mirror ops";
    return SUCCESS;
  }

  OperatorVector tmp_mirror_op;  // tmp mirror op for 'out_shape'
  mirror_ops_.push_back(tmp_mirror_op);
  return SUCCESS;
}

void Conv2DBackpropInputInfo::UpdateOutShape(const CNodePtr &cnode) {
  MS_EXCEPTION_IF_NULL(cnode);
  if (cnode->size() != 4) {
    MS_LOG(EXCEPTION) << name_ << ": The size of cnode's inputs must be 4, but got " << cnode->size();
  }

  if (!IsValueNode<ValueTuple>(cnode->input(3))) {
    MS_LOG(EXCEPTION) << name_ << ": The cnode's input[3] is not value node";
  }

  auto func_graph = cnode->func_graph();
  MS_EXCEPTION_IF_NULL(func_graph);
  auto manager = func_graph->manager();
  MS_EXCEPTION_IF_NULL(manager);

  ValuePtr out_shape = MakeValue(out_slice_shape_);
  AnfNodePtr val = NewValueNode(out_shape);
  (void)manager->Replace(cnode->input(3), val);
  MS_LOG(INFO) << name_ << ": Update the output shape " << out_slice_shape_;
}
}  // namespace parallel
}  // namespace mindspore
