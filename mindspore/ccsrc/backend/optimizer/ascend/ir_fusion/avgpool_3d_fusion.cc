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

#include "backend/optimizer/ascend/ir_fusion/avgpool_3d_fusion.h"
#include <vector>
#include <memory>
#include <string>
#include <algorithm>
#include "backend/session/anf_runtime_algorithm.h"
#include "backend/optimizer/common/helper.h"
#include "base/core_ops.h"
#include "utils/utils.h"

namespace mindspore {
namespace opt {
namespace {
constexpr size_t k5DInferDims = 5;
constexpr size_t kC0 = 16;

int64_t GetInterSection(int64_t start_1, int64_t end_1, int64_t start_2, int64_t end_2) {
  if (end_1 <= start_2) {
    return 0;
  }
  if (start_1 >= end_2) {
    return 0;
  }
  if (start_1 < start_2) {
    start_1 = start_2;
  }
  if (end_1 > end_2) {
    end_1 = end_2;
  }
  return end_1 - start_1;
}

bool GetKernelSize(const AnfNodePtr &node, int64_t *kd, int64_t *kh, int64_t *kw) {
  MS_EXCEPTION_IF_NULL(node);
  if (AnfAlgo::HasNodeAttr("kernel_size", node->cast<CNodePtr>())) {
    auto kernel_size = AnfAlgo::GetNodeAttr<std::vector<int64_t>>(node, "kernel_size");
    if (kernel_size.size() == 1) {
      *kd = kernel_size[0];
      *kh = kernel_size[0];
      *kw = kernel_size[0];
    } else if (kernel_size.size() == 3) {
      *kd = kernel_size[0];
      *kh = kernel_size[1];
      *kw = kernel_size[2];
    } else if (kernel_size.size() == 5) {
      // NCDHW
      *kd = kernel_size[2];
      *kh = kernel_size[3];
      *kw = kernel_size[4];
    } else {
      MS_LOG(EXCEPTION) << "Unknown kernel size " << kernel_size.size();
    }
    return true;
  }
  return false;
}

bool GetStrideSize(const AnfNodePtr &node, int64_t *sd, int64_t *sh, int64_t *sw) {
  MS_EXCEPTION_IF_NULL(node);
  if (AnfAlgo::HasNodeAttr("strides", node->cast<CNodePtr>())) {
    auto kernel_size = AnfAlgo::GetNodeAttr<std::vector<int64_t>>(node, "strides");
    if (kernel_size.size() == 1) {
      *sd = kernel_size[0];
      *sh = kernel_size[0];
      *sw = kernel_size[0];
    } else if (kernel_size.size() == 3) {
      *sd = kernel_size[0];
      *sh = kernel_size[1];
      *sw = kernel_size[2];
    } else if (kernel_size.size() == 5) {
      // NCDHW
      *sd = kernel_size[2];
      *sh = kernel_size[3];
      *sw = kernel_size[4];
    } else {
      MS_LOG(EXCEPTION) << "Unknown strides size " << kernel_size.size();
    }
    return true;
  }
  return false;
}

void GetAttrs(const AnfNodePtr &node, std::vector<int64_t> *pad_list, bool *count_include_pad, bool *ceil_mode,
              int64_t *divisor_override) {
  MS_EXCEPTION_IF_NULL(node);
  if (!AnfAlgo::HasNodeAttr("pad_list", node->cast<CNodePtr>())) {
    MS_LOG(EXCEPTION) << "AvgPool3D should has attr pad_list";
  }
  *pad_list = AnfAlgo::GetNodeAttr<std::vector<int64_t>>(node, "pad_list");
  if (AnfAlgo::HasNodeAttr("count_include_pad", node->cast<CNodePtr>())) {
    *count_include_pad = AnfAlgo::GetNodeAttr<bool>(node, "count_include_pad");
  }
  if (AnfAlgo::HasNodeAttr("ceil_mode", node->cast<CNodePtr>())) {
    *ceil_mode = AnfAlgo::GetNodeAttr<bool>(node, "ceil_mode");
  }
  if (AnfAlgo::HasNodeAttr("divisor_override", node->cast<CNodePtr>())) {
    *divisor_override = AnfAlgo::GetNodeAttr<int64_t>(node, "divisor_override");
  }
}

bool IsVectorImpl(int64_t fh, int64_t fw, int64_t kh, int64_t kw, const std::vector<int64_t> &pad_list) {
  if (std::any_of(pad_list.begin(), pad_list.end(), [](int64_t item) { return item != 0; })) {
    return false;
  }
  if (fh != kh || fw != kw) {
    return false;
  }
  return true;
}

bool IsZeroPads(const std::vector<int64_t> &pad_list) {
  return std::all_of(pad_list.begin(), pad_list.end(), [](int64_t item) { return item == 0; });
}

AnfNodePtr ConstructFilter(const FuncGraphPtr &func_graph, const std::vector<int64_t> &pad_list, int64_t fc, int64_t kd,
                           int64_t kh, int64_t kw, bool ceil_mode, int64_t divisor_override) {
  MS_EXCEPTION_IF_NULL(func_graph);
  //  assist tensor 1
  int64_t c1 = (fc + kC0 - 1) / kC0;
  std::vector<int64_t> assist_shape = {c1 * kd * kh * kw, 1, kC0, kC0};  // frac_z_3d
  auto infer_shape = {IntToSize(1), LongToSize(fc), LongToSize(kd), LongToSize(kh), LongToSize(kw)};
  float val = 1.0 / (kd * kh * kw);
  if (divisor_override) {
    val = 1.0 / divisor_override;
  } else if (!IsZeroPads(pad_list) || ceil_mode) {
    val = 1.0;
  }
  // create value node
  tensor::TensorPtr assist_tensor = std::make_shared<tensor::Tensor>(kNumberTypeFloat16, assist_shape);
  TensorTypePtr tensor_type = std::make_shared<TensorType>(kFloat16);
  tensor::DeviceInfo device_info{kOpFormat_FRACTAL_Z_3D, tensor_type, kOpFormat_FRACTAL_Z_3D};
  assist_tensor->set_device_info(device_info);
  auto tensor_data = reinterpret_cast<float16 *>(assist_tensor->data_c());
  int64_t cnt = c1 * kd * kh * kw;
  for (int64_t i = 0; i < cnt; ++i) {
    for (size_t j = 0; j < kC0; ++j) {
      for (size_t k = 0; k < kC0; ++k) {
        float t = j == k ? val : 0;
        *tensor_data = float16(t);
        ++tensor_data;
      }
    }
  }

  auto x_abstract = std::make_shared<abstract::AbstractTensor>(kFloat16, assist_shape);
  auto kernel_graph = func_graph->cast<KernelGraphPtr>();
  auto value_node = kernel_graph->NewValueNode(x_abstract, assist_tensor);
  kernel_graph->AddValueNodeToGraph(value_node);
  AnfAlgo::SetOutputInferTypeAndShape({kNumberTypeFloat16}, {infer_shape}, value_node.get());
  return value_node;
}

AnfNodePtr ConstructMultiplier(const FuncGraphPtr &func_graph, int64_t fn, int64_t fc, int64_t fd, int64_t fh,
                               int64_t fw, int64_t dd, int64_t dh, int64_t dw, int64_t kd, int64_t kh, int64_t kw,
                               int64_t sd, int64_t sh, int64_t sw, const std::vector<int64_t> &pad_list, bool ceil_mode,
                               bool count_include_pad) {
  MS_EXCEPTION_IF_NULL(func_graph);
  //  assist tensor 2
  std::vector<int64_t> assist_shape = {fn, fc, dd, dh, dw};  // NCDHW
  auto infer_shape = {LongToSize(fn), LongToSize(fc), LongToSize(dd), LongToSize(dh), LongToSize(dw)};
  tensor::TensorPtr tensor = std::make_shared<tensor::Tensor>(kNumberTypeFloat16, assist_shape);
  auto tensor_data = reinterpret_cast<float16 *>(tensor->data_c());
  auto pad_d = pad_list[0] + pad_list[1];
  auto pad_h = pad_list[2] + pad_list[3];
  auto pad_w = pad_list[4] + pad_list[5];
  auto len_d = fd + pad_d;
  auto len_h = fh + pad_h;
  auto len_w = fw + pad_w;
  for (int64_t nn = 0; nn < fn; nn++) {
    for (int64_t cc = 0; cc < fc; cc++) {
      int64_t start_d = 0;
      for (int64_t di = 0; di < dd; di++) {
        auto v_kd = start_d + kd <= len_d ? kd : len_d - start_d;
        int64_t start_h = 0;
        for (int64_t hi = 0; hi < dh; hi++) {
          auto v_kh = start_h + kh <= len_h ? kh : len_h - start_h;
          int64_t start_w = 0;
          for (int64_t wi = 0; wi < dw; wi++) {
            auto v_kw = start_w + kw < len_w ? kw : len_w - start_w;
            auto vaild_d = GetInterSection(start_d, start_d + kd, pad_list[0], pad_list[0] + fd);
            auto vaild_h = GetInterSection(start_h, start_h + kh, pad_list[2], pad_list[2] + fh);
            auto vaild_w = GetInterSection(start_w, start_w + kw, pad_list[4], pad_list[4] + fw);
            auto vaild_data = vaild_d * vaild_h * vaild_w;
            auto vaild_kernel = v_kd * v_kh * v_kw;
            float val = count_include_pad ? 1.0 / vaild_kernel : 1.0 / vaild_data;
            *tensor_data = float16(val);
            ++tensor_data;
            start_w += sw;
          }
          start_h += sh;
        }
        start_d += sd;
      }
    }
  }
  auto x_abstract = std::make_shared<abstract::AbstractTensor>(kFloat16, assist_shape);
  auto kernel_graph = func_graph->cast<KernelGraphPtr>();
  auto value_node = kernel_graph->NewValueNode(x_abstract, tensor);
  kernel_graph->AddValueNodeToGraph(value_node);
  AnfAlgo::SetOutputInferTypeAndShape({kNumberTypeFloat16}, {infer_shape}, value_node.get());
  return value_node;
}
}  // namespace

const BaseRef AvgPool3DFusion::DefinePattern() const {
  VarPtr Xs = std::make_shared<SeqVar>();
  return VectorRef({prim::kPrimAvgPool3D, Xs});
}

const AnfNodePtr AvgPool3DFusion::Process(const FuncGraphPtr &func_graph, const AnfNodePtr &node,
                                          const EquivPtr &) const {
  MS_EXCEPTION_IF_NULL(func_graph);
  MS_EXCEPTION_IF_NULL(node);
  auto avg_pool_3d_node = node->cast<CNodePtr>();
  MS_EXCEPTION_IF_NULL(avg_pool_3d_node);
  auto dims_in = AnfAlgo::GetPrevNodeOutputInferShape(avg_pool_3d_node, 0);
  auto dims_out = AnfAlgo::GetOutputInferShape(avg_pool_3d_node, 0);
  if (dims_in.size() < k5DInferDims || dims_out.size() < k5DInferDims) {
    MS_LOG(EXCEPTION) << "AvgPool3D's in_out infer shape dims can not be less " << k5DInferDims;
  }
  auto fn = SizeToLong(dims_in[0]);
  auto fc = SizeToLong(dims_in[1]);
  auto fd = SizeToLong(dims_in[2]);
  auto fh = SizeToLong(dims_in[3]);
  auto fw = SizeToLong(dims_in[4]);
  auto dout = SizeToLong(dims_out[2]);
  auto dh = SizeToLong(dims_out[3]);
  auto dw = SizeToLong(dims_out[4]);
  // kernel size
  int64_t kd;
  int64_t kh;
  int64_t kw;
  if (!GetKernelSize(avg_pool_3d_node, &kd, &kh, &kw)) {
    MS_LOG(EXCEPTION) << "GetK kernel size failed";
  }
  // strides
  int64_t sd;
  int64_t sh;
  int64_t sw;
  if (!GetStrideSize(avg_pool_3d_node, &sd, &sh, &sw)) {
    MS_LOG(EXCEPTION) << "GetK stride size failed";
  }
  std::vector<int64_t> pad_list;
  bool count_include_pad = false;
  bool ceil_mode = false;
  int64_t divisor_override = 0;
  GetAttrs(avg_pool_3d_node, &pad_list, &count_include_pad, &ceil_mode, &divisor_override);
  if (IsVectorImpl(fh, fw, kh, kw, pad_list)) {
    MS_LOG(INFO) << "No need fusion";
    return nullptr;
  }
  std::vector<AnfNodePtr> new_inputs{NewValueNode(std::make_shared<Primitive>(prim::kPrimAvgPool3D->name()))};
  (void)new_inputs.insert(new_inputs.end(), avg_pool_3d_node->inputs().begin() + 1, avg_pool_3d_node->inputs().end());
  // assist node 1
  auto filter_node = ConstructFilter(func_graph, pad_list, fc, kd, kh, kw, ceil_mode, divisor_override);
  new_inputs.push_back(filter_node);
  MS_EXCEPTION_IF_NULL(filter_node);
  // assist node 2
  if ((!IsZeroPads(pad_list) || ceil_mode) && !divisor_override) {
    auto multiplier = ConstructMultiplier(func_graph, fn, fc, fd, fh, fw, dout, dh, dw, kd, kh, kw, sd, sh, sw,
                                          pad_list, ceil_mode, count_include_pad);
    new_inputs.push_back(multiplier);
  }
  auto new_3d = func_graph->NewCNode(new_inputs);
  MS_EXCEPTION_IF_NULL(new_3d);
  new_3d->set_scope(avg_pool_3d_node->scope());
  new_3d->set_abstract(avg_pool_3d_node->abstract());
  AnfAlgo::CopyNodeAttrs(avg_pool_3d_node, new_3d);
  return new_3d;
}
}  // namespace opt
}  // namespace mindspore
