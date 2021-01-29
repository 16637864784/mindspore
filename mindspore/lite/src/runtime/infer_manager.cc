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
#include "src/runtime/infer_manager.h"
#include "src/common/tensor_util.h"
#include "schema/model_generated.h"
#include "nnacl/infer/common_infer.h"
#include "nnacl/infer/adam_infer.h"
#include "nnacl/infer/addn_infer.h"
#include "nnacl/infer/apply_momentum_infer.h"
#include "nnacl/infer/argmax_infer.h"
#include "nnacl/infer/argmin_infer.h"
#include "nnacl/infer/arithmetic_compare_infer.h"
#include "nnacl/infer/arithmetic_grad_infer.h"
#include "nnacl/infer/arithmetic_infer.h"
#include "nnacl/infer/assign_add_infer.h"
#include "nnacl/infer/assign_infer.h"
#include "nnacl/infer/audio_spectrogram_infer.h"
#include "nnacl/infer/batch_to_space_infer.h"
#include "nnacl/infer/bias_grad_infer.h"
#include "nnacl/infer/binary_cross_entropy_infer.h"
#include "nnacl/infer/bn_grad_infer.h"
#include "nnacl/infer/broadcast_to_infer.h"
#include "nnacl/infer/cast_infer.h"
#include "nnacl/infer/concat_infer.h"
#include "nnacl/infer/constant_of_shape_infer.h"
#include "nnacl/infer/conv2d_grad_filter_infer.h"
#include "nnacl/infer/conv2d_grad_input_infer.h"
#include "nnacl/infer/conv2d_infer.h"
#include "nnacl/infer/crop_infer.h"
#include "nnacl/infer/custom_extract_features_infer.h"
#include "nnacl/infer/custom_normalize_infer.h"
#include "nnacl/infer/custom_predict_infer.h"
#include "nnacl/infer/deconv2d_infer.h"
#include "nnacl/infer/dedepthwise_conv2d_infer.h"
#include "nnacl/infer/depth_to_space_infer.h"
#include "nnacl/infer/depthwise_conv2d_infer.h"
#include "nnacl/infer/detection_post_process_infer.h"
#include "nnacl/infer/dropout_grad_infer.h"
#include "nnacl/infer/embedding_lookup_infer.h"
#include "nnacl/infer/expand_dims_infer.h"
#include "nnacl/infer/fft_imag_infer.h"
#include "nnacl/infer/fft_real_infer.h"
#include "nnacl/infer/fill_infer.h"
#include "nnacl/infer/flatten_grad_infer.h"
#include "nnacl/infer/flatten_infer.h"
#include "nnacl/infer/full_connection_infer.h"
#include "nnacl/infer/fused_batchnorm_infer.h"
#include "nnacl/infer/gather_infer.h"
#include "nnacl/infer/gather_nd_infer.h"
#include "nnacl/infer/group_conv2d_grad_input_infer.h"
#include "nnacl/infer/hashtable_lookup_infer.h"
#include "nnacl/infer/layer_norm_infer.h"
#include "nnacl/infer/lsh_projection_infer.h"
#include "nnacl/infer/lstm_infer.h"
#include "nnacl/infer/matmul_infer.h"
#include "nnacl/infer/maximum_grad_infer.h"
#include "nnacl/infer/mean_infer.h"
#include "nnacl/infer/mfcc_infer.h"
#include "nnacl/infer/nchw2nhwc_infer.h"
#include "nnacl/infer/nhwc2nchw_infer.h"
#include "nnacl/infer/non_max_suppression_infer.h"
#include "nnacl/infer/one_hot_infer.h"
#include "nnacl/infer/pad_infer.h"
#include "nnacl/infer/pooling_grad_infer.h"
#include "nnacl/infer/pooling_infer.h"
#include "nnacl/infer/power_infer.h"
#include "nnacl/infer/quant_dtype_cast_infer.h"
#include "nnacl/infer/range_infer.h"
#include "nnacl/infer/rank_infer.h"
#include "nnacl/infer/reduce_infer.h"
#include "nnacl/infer/reshape_infer.h"
#include "nnacl/infer/resize_infer.h"
#include "nnacl/infer/rfft_infer.h"
#include "nnacl/infer/roi_pooling_infer.h"
#include "nnacl/infer/scatter_nd_infer.h"
#include "nnacl/infer/sgd_infer.h"
#include "nnacl/infer/shape_infer.h"
#include "nnacl/infer/skip_gram_infer.h"
#include "nnacl/infer/slice_infer.h"
#include "nnacl/infer/softmax_cross_entropy_infer.h"
#include "nnacl/infer/softmax_infer.h"
#include "nnacl/infer/space_to_batch_infer.h"
#include "nnacl/infer/space_to_batch_nd_infer.h"
#include "nnacl/infer/space_to_depth_infer.h"
#include "nnacl/infer/sparse_to_dense_infer.h"
#include "nnacl/infer/split_infer.h"
#include "nnacl/infer/squeeze_infer.h"
#include "nnacl/infer/stack_infer.h"
#include "nnacl/infer/strided_slice_infer.h"
#include "nnacl/infer/tile_infer.h"
#include "nnacl/infer/topk_infer.h"
#include "nnacl/infer/transpose_infer.h"
#include "nnacl/infer/unique_infer.h"
#include "nnacl/infer/unsorted_segment_sum_infer.h"
#include "nnacl/infer/unsqueeze_infer.h"
#include "nnacl/infer/unstack_infer.h"
#include "nnacl/infer/where_infer.h"
#include "nnacl/infer/while_infer.h"
#include "include/errorcode.h"
#include "nnacl/errorcode.h"

#include "src/tensorlist.h"
#include "nnacl/infer/tensorlist_reserve_infer.h"
#include "nnacl/infer/tensorlist_getitem_infer.h"
#include "nnacl/infer/tensorlist_fromtensor_infer.h"
#include "nnacl/infer/tensorlist_setitem_infer.h"
#include "nnacl/infer/tensorlist_stack_infer.h"
#include "nnacl/infer/partial_infer.h"
#include "nnacl/infer/merge_infer.h"
#include "nnacl/infer/switch_infer.h"
#include "nnacl/infer/assert_op_infer.h"
#include "nnacl/infer/sparse_softmax_cross_entropy_infer.h"
#include "nnacl/infer/dropout_infer.h"
#include "nnacl/infer/prior_box_infer.h"

namespace mindspore {
namespace lite {

TensorC *NewTensorC() {
  auto *tensor_c = static_cast<TensorC *>(malloc(sizeof(TensorC)));
  if (tensor_c == nullptr) {
    MS_LOG(ERROR) << "malloc tensor fail!";
    return nullptr;
  }
  tensor_c->data_type_ = kNumberTypeFloat32;
  tensor_c->format_ = schema::Format::Format_NCHW;
  tensor_c->data_ = nullptr;
  tensor_c->shape_size_ = 0;
  return tensor_c;
}

void Tensor2TensorC(Tensor *src, TensorC *dst) {
  dst->format_ = src->format();
  dst->data_ = src->data_c();
  dst->data_type_ = src->data_type();
  dst->shape_size_ = src->shape().size();
  for (size_t i = 0; i < dst->shape_size_; i++) {
    dst->shape_[i] = src->shape().at(i);
  }
}

void TensorC2Tensor(TensorC *src, Tensor *dst) {
  dst->set_format(static_cast<schema::Format>(src->format_));
  dst->set_data(src->data_);
  dst->set_data_type(static_cast<TypeId>(src->data_type_));
  dst->set_shape(std::vector<int>(src->shape_, src->shape_ + src->shape_size_));
}

void TensorList2TensorListC(TensorList *src, TensorListC *dst) {
  dst->data_type_ = src->data_type();
  dst->format_ = src->format();
  dst->element_num_ = src->shape().empty() ? 0 : src->shape().at(0);

  for (size_t i = 0; i < dst->element_num_; i++) {
    if (dst->tensors_[i] == nullptr) {
      dst->tensors_[i] = reinterpret_cast<TensorC *>(malloc(sizeof(TensorC)));
    }
    Tensor2TensorC(src->tensors().at(i), dst->tensors_[i]);  // note: use pushback?
  }

  dst->tensors_data_type_ = src->tensors_data_type();
  dst->element_shape_size_ = src->element_shape().size();
  for (size_t i = 0; i < dst->element_shape_size_; i++) {
    dst->element_shape_[i] = src->element_shape().at(i);
  }
  dst->max_elements_num_ = src->max_elements_num();
}

void TensorListC2TensorList(TensorListC *src, TensorList *dst) {
  dst->set_data_type(static_cast<TypeId>(src->data_type_));
  dst->set_format(static_cast<schema::Format>(src->format_));
  dst->set_shape(std::vector<int>(1, src->element_num_));
  dst->set_tensors_data_type(static_cast<TypeId>(src->tensors_data_type_));

  // Set Tensors
  for (size_t i = 0; i < src->element_num_; i++) {
    Tensor *tmp = new Tensor;
    TensorC2Tensor(src->tensors_[i], tmp);
    dst->SetTensor(i, tmp);
  }

  dst->set_element_shape(std::vector<int>(src->element_shape_, src->element_shape_ + src->element_shape_size_));
  dst->set_max_elements_num(src->max_elements_num_);
}

int GenerateMergeOutTensorC(const std::vector<lite::Tensor *> &inputs, std::vector<lite::Tensor *> *outputs,
                            std::vector<TensorC *> *out_tensor_c) {
  int ret = RET_OK;
  for (size_t i = 0; i < outputs->size(); i++) {
    if (inputs.at(i)->data_type() == kObjectTypeTensorType) {
      auto *output_tensorlist = malloc(sizeof(TensorListC));
      if (output_tensorlist == nullptr) {
        MS_LOG(ERROR) << "malloc tensorlist_c failed";
        ret = RET_ERROR;
        break;
      }
      out_tensor_c->push_back(reinterpret_cast<TensorC *const>(output_tensorlist));
    } else {
      auto *output_tensor = NewTensorC();
      if (output_tensor == nullptr) {
        MS_LOG(ERROR) << "malloc tensor_c failed";
        ret = RET_ERROR;
        break;
      }
      out_tensor_c->push_back(reinterpret_cast<TensorC *const>(output_tensor));
    }
  }
  return ret;
}

int GenerateSwitchOutTensorC(const std::vector<lite::Tensor *> &inputs, std::vector<lite::Tensor *> *outputs,
                             std::vector<TensorC *> *out_tensor_c) {
  int ret = RET_OK;
  MS_ASSERT(inputs.size() == outputs->size() / 2 + 1);
  out_tensor_c->resize(outputs->size());
  for (size_t i = 0; i < outputs->size() / 2; i++) {
    if (inputs.at(i + 1)->data_type() == kObjectTypeTensorType) {
      auto *output_tensorlist1 = malloc(sizeof(TensorListC));
      if (output_tensorlist1 == nullptr) {
        MS_LOG(ERROR) << "malloc tensorlist_c failed";
        ret = RET_ERROR;
        break;
      }
      out_tensor_c->at(i) = reinterpret_cast<TensorC *const>(output_tensorlist1);
      auto *output_tensorlist2 = malloc(sizeof(TensorListC));
      if (output_tensorlist2 == nullptr) {
        MS_LOG(ERROR) << "malloc tensorlist_c failed";
        ret = RET_ERROR;
        break;
      }
      out_tensor_c->at(i + outputs->size() / 2) = reinterpret_cast<TensorC *const>(output_tensorlist2);
    } else {
      auto *output_tensor1 = NewTensorC();
      if (output_tensor1 == nullptr) {
        MS_LOG(ERROR) << "malloc tensor_c failed";
        ret = RET_ERROR;
        break;
      }
      out_tensor_c->at(i) = reinterpret_cast<TensorC *const>(output_tensor1);
      auto *output_tensor2 = NewTensorC();
      if (output_tensor2 == nullptr) {
        MS_LOG(ERROR) << "malloc tensor_c failed";
        ret = RET_ERROR;
        break;
      }
      out_tensor_c->at(i + outputs->size() / 2) = reinterpret_cast<TensorC *const>(output_tensor2);
    }
  }
  return ret;
}

int GenerateOutTensorC(const OpParameter *const parameter, const std::vector<lite::Tensor *> &inputs,
                       std::vector<lite::Tensor *> *outputs, std::vector<TensorC *> *out_tensor_c) {
  int ret = RET_OK;
  if (parameter->type_ == mindspore::schema::PrimitiveType_TensorListFromTensor ||
      parameter->type_ == mindspore::schema::PrimitiveType_TensorListReserve ||
      parameter->type_ == mindspore::schema::PrimitiveType_TensorListSetItem) {
    // TensorListC ->TensorC
    auto *tensor_list_c = reinterpret_cast<TensorListC *>(malloc(sizeof(TensorListC)));  // note: malloc or new ?
    if (tensor_list_c == nullptr) {
      ret = RET_ERROR;
    } else {
      out_tensor_c->push_back(reinterpret_cast<TensorC *const>(tensor_list_c));
    }
  } else if (parameter->type_ == mindspore::schema::PrimitiveType_Merge) {
    ret = GenerateMergeOutTensorC(inputs, outputs, out_tensor_c);
  } else if (parameter->type_ == mindspore::schema::PrimitiveType_Switch) {
    ret = GenerateSwitchOutTensorC(inputs, outputs, out_tensor_c);
  } else {
    ret = OutputTensor2TensorC(*outputs, out_tensor_c);
  }
  return ret;
}

int KernelInferShape(const std::vector<lite::Tensor *> &inputs, std::vector<lite::Tensor *> *outputs,
                     OpParameter *parameter) {
  std::vector<TensorC *> in_tensors;
  std::vector<TensorC *> out_tensors;

  int ret = 0;
  for (auto input : inputs) {
    if (input->data_type() == kObjectTypeTensorType) {
      // Tensor ->TensorList -> TensorListC -> TensorC
      auto *tensor_list = reinterpret_cast<TensorList *>(input);
      auto *tensor_list_c = reinterpret_cast<TensorListC *>(malloc(sizeof(TensorListC)));
      if (tensor_list_c == nullptr) {
        ret = RET_NULL_PTR;
        break;
      }
      memset(tensor_list_c, 0, sizeof(TensorListC));
      TensorList2TensorListC(tensor_list, tensor_list_c);
      in_tensors.push_back(reinterpret_cast<TensorC *>(tensor_list_c));  // in_tensors[0]
    } else {
      // Tensor -> TensorC
      auto *tensor_c = reinterpret_cast<TensorC *>(malloc(sizeof(TensorC)));
      if (tensor_c == nullptr) {
        ret = RET_NULL_PTR;
        break;
      }
      Tensor2TensorC(input, tensor_c);
      in_tensors.emplace_back(tensor_c);
    }
  }

  if (ret != RET_OK) {
    FreeAllTensorC(&in_tensors);
    return RET_ERROR;
  }

  ret = GenerateOutTensorC(parameter, inputs, outputs, &out_tensors);

  if (ret != RET_OK) {
    FreeAllTensorC(&in_tensors);
    FreeAllTensorC(&out_tensors);
    return RET_ERROR;
  }
  auto infer_shape_func = InferManager::GetInstance()->GetInferShapeFunc(parameter->type_);
  if (infer_shape_func == nullptr) {
    MS_LOG(ERROR) << "Get infershape func failed! type:" << PrimitiveCurVersionTypeName(parameter->type_);
    return RET_ERROR;
  }
  ret = infer_shape_func(static_cast<TensorC **>(in_tensors.data()), in_tensors.size(), out_tensors.data(),
                         out_tensors.size(), parameter);

  if (ret == RET_OK) {
    for (size_t i = 0; i < out_tensors.size(); i++) {
      if (reinterpret_cast<TensorListC *>(out_tensors.at(i))->data_type_ == TypeIdC::kObjectTypeTensorType) {
        // TensorC -> TensorListC -> TensorList -> Tensor
        auto *tensor_list_c = reinterpret_cast<TensorListC *>(out_tensors.at(i));
        auto *tensor_list = reinterpret_cast<TensorList *>(outputs->at(i));
        tensor_list->set_shape({static_cast<int>(tensor_list_c->element_num_)});
        auto tensor_shape = std::vector<std::vector<int>>(
          tensor_list_c->element_num_,
          std::vector<int>(tensor_list_c->element_shape_,
                           tensor_list_c->element_shape_ + tensor_list_c->element_shape_size_));
        tensor_list->MallocTensorListData(static_cast<TypeId>(tensor_list_c->data_type_), tensor_shape);
        TensorListC2TensorList(tensor_list_c, tensor_list);
      } else {
        TensorC2Tensor(out_tensors.at(i), outputs->at(i));
      }
    }
  } else {
    TensorC2LiteTensor(out_tensors, outputs);
  }

  FreeAllTensorC(&in_tensors);
  FreeAllTensorC(&out_tensors);
  if (ret == NNACL_INFER_INVALID) {
    return RET_INFER_INVALID;
  } else if (ret != NNACL_OK) {
    return RET_INFER_ERR;
  }
  return RET_OK;
}

static RegistryInferShape g_TopkInferShape(mindspore::schema::PrimitiveType_TopKFusion, TopKInferShape);
static RegistryInferShape g_MaxPoolingInferShape(mindspore::schema::PrimitiveType_MaxPoolFusion, PoolingInferShape);
static RegistryInferShape g_AvgPoolingInferShape(mindspore::schema::PrimitiveType_AvgPoolFusion, PoolingInferShape);
static RegistryInferShape g_DetectionPostProcessInferShape(mindspore::schema::PrimitiveType_DetectionPostProcess,
                                                           DetectionPostProcessInferShape);
static RegistryInferShape g_SpaceToBatchNdInferShape(mindspore::schema::PrimitiveType_SpaceToBatchND,
                                                     SpaceToBatchNdInferShape);
static RegistryInferShape g_ScatterNdInferShape(mindspore::schema::PrimitiveType_ScatterNd, ScatterNdInferShape);
static RegistryInferShape g_FftRealInferShape(mindspore::schema::PrimitiveType_FftReal, FftRealInferShape);
static RegistryInferShape g_SpaceToBatchInferShape(mindspore::schema::PrimitiveType_SpaceToBatch,
                                                   SpaceToBatchInferShape);
static RegistryInferShape g_CustomPredictInferShape(mindspore::schema::PrimitiveType_CustomPredict,
                                                    CustomPredictInferShape);
static RegistryInferShape g_Conv2dInferShape(mindspore::schema::PrimitiveType_Conv2DFusion, Conv2dInferShape);
static RegistryInferShape g_Deconv2dInferShape(mindspore::schema::PrimitiveType_Conv2dTransposeFusion,
                                               Deconv2dInferShape);
static RegistryInferShape g_SquaredDifferenceInferShape(mindspore::schema::PrimitiveType_SquaredDifference,
                                                        ArithmeticInferShape);
static RegistryInferShape g_AddInferShape(mindspore::schema::PrimitiveType_AddFusion, ArithmeticInferShape);
static RegistryInferShape g_SubInferShape(mindspore::schema::PrimitiveType_SubFusion, ArithmeticInferShape);
static RegistryInferShape g_DivInferShape(mindspore::schema::PrimitiveType_DivFusion, ArithmeticInferShape);
static RegistryInferShape g_MulInferShape(mindspore::schema::PrimitiveType_MulFusion, ArithmeticInferShape);
static RegistryInferShape g_FloorDivInferShape(mindspore::schema::PrimitiveType_FloorDiv, ArithmeticInferShape);
static RegistryInferShape g_RealDivInferShape(mindspore::schema::PrimitiveType_RealDiv, ArithmeticInferShape);
static RegistryInferShape g_LogicalOrInferShape(mindspore::schema::PrimitiveType_LogicalOr, ArithmeticInferShape);
static RegistryInferShape g_LogicalAndInferShape(mindspore::schema::PrimitiveType_LogicalAnd, ArithmeticInferShape);
static RegistryInferShape g_MinuimumInferShape(mindspore::schema::PrimitiveType_Minimum, ArithmeticInferShape);
static RegistryInferShape g_MaximumInferShape(mindspore::schema::PrimitiveType_Maximum, ArithmeticInferShape);
static RegistryInferShape g_FloorModInferShape(mindspore::schema::PrimitiveType_FloorMod, ArithmeticInferShape);
static RegistryInferShape g_EltwiseInferShape(mindspore::schema::PrimitiveType_Eltwise, ArithmeticInferShape);

static RegistryInferShape g_SpaceToDepthInferShape(mindspore::schema::PrimitiveType_SpaceToDepth,
                                                   SpaceToDepthInferShape);
static RegistryInferShape g_Conv2dGradFilterInferShape(mindspore::schema::PrimitiveType_Conv2DBackpropFilterFusion,
                                                       Conv2dGradFilterInferShape);
static RegistryInferShape g_PadInferShape(mindspore::schema::PrimitiveType_PadFusion, PadInferShape);
static RegistryInferShape g_ApplyMomentumInferShape(mindspore::schema::PrimitiveType_ApplyMomentum,
                                                    ApplyMomentumInferShape);
static RegistryInferShape g_GatherInferShape(mindspore::schema::PrimitiveType_Gather, GatherInferShape);
static RegistryInferShape g_SkipGramInferShape(mindspore::schema::PrimitiveType_SkipGram, SkipGramInferShape);
static RegistryInferShape g_StridedSliceInferShape(mindspore::schema::PrimitiveType_StridedSlice,
                                                   StridedSliceInferShape);
static RegistryInferShape g_StackInferShape(mindspore::schema::PrimitiveType_Stack, StackInferShape);

// note: this will be added
// static RegistryInferShape g_ArithmeticGradInferShape(mindspore::schema::PrimitiveType_ArithmeticGrad,
// ArithmeticGradInferShape);

static RegistryInferShape g_AssignInferShape(mindspore::schema::PrimitiveType_Assign, AssignInferShape);
static RegistryInferShape g_BnGradInferShape(mindspore::schema::PrimitiveType_BatchNormGrad, BnGradInferShape);
static RegistryInferShape g_SplitInferShape(mindspore::schema::PrimitiveType_Split, SplitInferShape);
static RegistryInferShape g_HashtableLookupInferShape(mindspore::schema::PrimitiveType_HashtableLookup,
                                                      HashtableLoopupInferShape);
static RegistryInferShape g_FillInferShape(mindspore::schema::PrimitiveType_Fill, FillInferShape);
static RegistryInferShape g_MatmulInferShape(mindspore::schema::PrimitiveType_MatMul, MatmulInferShape);
static RegistryInferShape g_BatchToSpaceInferShape(mindspore::schema::PrimitiveType_BatchToSpace,
                                                   BatchToSpaceInferShape);
static RegistryInferShape g_RankInferShape(mindspore::schema::PrimitiveType_Rank, RankInferShape);
static RegistryInferShape g_FlattenGradInferShape(mindspore::schema::PrimitiveType_FlattenGrad, FlattenGradInferShape);
static RegistryInferShape g_ConcatInferShape(mindspore::schema::PrimitiveType_Concat, ConcatInferShape);
static RegistryInferShape g_SliceInferShape(mindspore::schema::PrimitiveType_SliceFusion, SliceInferShape);
static RegistryInferShape g_ExpandDimsInferShape(mindspore::schema::PrimitiveType_ExpandDims, ExpandDimsInferShape);
static RegistryInferShape g_ResizeInferShape(mindspore::schema::PrimitiveType_Resize, ResizeInferShape);
static RegistryInferShape g_WhereInferShape(mindspore::schema::PrimitiveType_Where, WhereInferShape);
static RegistryInferShape g_ConstantOfShapeInferShape(mindspore::schema::PrimitiveType_ConstantOfShape,
                                                      ConstantOfShapeInferShape);
static RegistryInferShape g_DepthToSpaceInferShape(mindspore::schema::PrimitiveType_DepthToSpace,
                                                   DepthToSpaceInferShape);
static RegistryInferShape g_SqueezeInferShape(mindspore::schema::PrimitiveType_Squeeze, SqueezeInferShape);
static RegistryInferShape g_RfftInferShape(mindspore::schema::PrimitiveType_Rfft, RfftInferShape);
static RegistryInferShape g_CastInferShape(mindspore::schema::PrimitiveType_Cast, CastInferShape);
static RegistryInferShape g_SparseToDenseInferShape(mindspore::schema::PrimitiveType_SparseToDense,
                                                    SparseToDenseInferShape);
static RegistryInferShape g_Conv2dGradInputInferShape(mindspore::schema::PrimitiveType_Conv2DBackpropInputFusion,
                                                      Conv2dGradInputInferShape);
static RegistryInferShape g_QuantDtypeCastInferShape(mindspore::schema::PrimitiveType_QuantDTypeCast,
                                                     QuantDtypeCastInferShape);
static RegistryInferShape g_MfccInferShape(mindspore::schema::PrimitiveType_Mfcc, MfccInferShape);
static RegistryInferShape g_AssignAddInferShape(mindspore::schema::PrimitiveType_AssignAdd, AssignAddInferShape);
static RegistryInferShape g_LayerNormInferShape(mindspore::schema::PrimitiveType_LayerNormFusion, LayerNormInferShape);
static RegistryInferShape g_UnsortedSegmentSumInferShape(mindspore::schema::PrimitiveType_UnsortedSegmentSum,
                                                         UnsortedSegmentSumInferShape);
static RegistryInferShape g_AddnInferShape(mindspore::schema::PrimitiveType_AddN, AddnInferShape);
static RegistryInferShape g_BiasGradInferShape(mindspore::schema::PrimitiveType_BiasGrad, BiasGradInferShape);
static RegistryInferShape g_FullConnectionInferShape(mindspore::schema::PrimitiveType_FullConnection,
                                                     FullConnectionInferShape);
static RegistryInferShape g_CropInferShape(mindspore::schema::PrimitiveType_Crop, CropInferShape);
static RegistryInferShape g_DropoutGradInferShape(mindspore::schema::PrimitiveType_DropoutGrad, DropoutGradInferShape);
static RegistryInferShape g_AdamInferShape(mindspore::schema::PrimitiveType_Adam, AdamInferShape);
static RegistryInferShape g_FusedBatchnormInferShape(mindspore::schema::PrimitiveType_FusedBatchNorm,
                                                     FusedBatchNormInferShape);
static RegistryInferShape g_SoftmaxInferShape(mindspore::schema::PrimitiveType_Softmax, SoftMaxInferShape);
static RegistryInferShape g_RoiPoolingInferShape(mindspore::schema::PrimitiveType_ROIPooling, ROIPoolingInferShape);
static RegistryInferShape g_PoolingGradInferShape(mindspore::schema::PrimitiveType_PoolingGrad, PoolingGradInferShape);
static RegistryInferShape g_WhileInferShape(mindspore::schema::PrimitiveType_While, WhileInferShape);
static RegistryInferShape g_BinaryCrossEntropyInferShape(mindspore::schema::PrimitiveType_BinaryCrossEntropy,
                                                         BinaryCrossEntropyInferShape);
static RegistryInferShape g_TileInferShape(mindspore::schema::PrimitiveType_TileFusion, TileInferShape);
static RegistryInferShape g_EmbeddingLookupInferShape(mindspore::schema::PrimitiveType_EmbeddingLookupFusion,
                                                      EmbeddingLookupInferShape);
static RegistryInferShape g_UnsqueezeInferShape(mindspore::schema::PrimitiveType_Unsqueeze, UnsqueezeInferShape);
static RegistryInferShape g_TransposeInferShape(mindspore::schema::PrimitiveType_Transpose, TransposeInferShape);
static RegistryInferShape g_GatherNdInferShape(mindspore::schema::PrimitiveType_GatherNd, GatherNdInferShape);
static RegistryInferShape g_BroadcastToInferShape(mindspore::schema::PrimitiveType_BroadcastTo, BroadcastToInferShape);
static RegistryInferShape g_MaximumGradInferShape(mindspore::schema::PrimitiveType_MaximumGrad, MaximumGradInferShape);
static RegistryInferShape g_PowerInferShape(mindspore::schema::PrimitiveType_PowFusion, PowerInferShape);
static RegistryInferShape g_RangeInferShape(mindspore::schema::PrimitiveType_Range, RangeInferShape);
static RegistryInferShape g_SgdInferShape(mindspore::schema::PrimitiveType_SGD, SgdInferShape);
static RegistryInferShape g_ArgminInferShape(mindspore::schema::PrimitiveType_ArgMinFusion, ArgminInferShape);
static RegistryInferShape g_UnstackInferShape(mindspore::schema::PrimitiveType_Unstack, UnstackInferShape);
static RegistryInferShape g_AudioSpectrogramInferShape(mindspore::schema::PrimitiveType_AudioSpectrogram,
                                                       AudioSpectrogramInferShape);

// note: no arithmetic_self
static RegistryInferShape g_BinaryCrossEntropyGradInferShape(mindspore::schema::PrimitiveType_BinaryCrossEntropyGrad,
                                                             CommonInferShape);
static RegistryInferShape g_ReverseSequenceInferShape(mindspore::schema::PrimitiveType_ReverseSequence,
                                                      CommonInferShape);
static RegistryInferShape g_ZerosLikeInferShape(mindspore::schema::PrimitiveType_ZerosLike, CommonInferShape);

static RegistryInferShape g_AbsInferShape(mindspore::schema::PrimitiveType_Abs, CommonInferShape);
static RegistryInferShape g_ActivationGradInferShape(mindspore::schema::PrimitiveType_ActivationGrad, CommonInferShape);
static RegistryInferShape g_ActivationInferShape(mindspore::schema::PrimitiveType_Activation, CommonInferShape);
static RegistryInferShape g_BatchNormInferShape(mindspore::schema::PrimitiveType_BatchNorm, CommonInferShape);
static RegistryInferShape g_BiasAddInferShape(mindspore::schema::PrimitiveType_BiasAdd, CommonInferShape);
static RegistryInferShape g_CeilInferShape(mindspore::schema::PrimitiveType_Ceil, CommonInferShape);
static RegistryInferShape g_ClipInferShape(mindspore::schema::PrimitiveType_Clip, CommonInferShape);
static RegistryInferShape g_CosInferShape(mindspore::schema::PrimitiveType_Cos, CommonInferShape);
static RegistryInferShape g_SinInferShape(mindspore::schema::PrimitiveType_Sin, CommonInferShape);
static RegistryInferShape g_DependInferShape(mindspore::schema::PrimitiveType_Depend, CommonInferShape);
// note : no Primitive_Dequant
static RegistryInferShape g_EluInferShape(mindspore::schema::PrimitiveType_Elu, CommonInferShape);
static RegistryInferShape g_ExpInferShape(mindspore::schema::PrimitiveType_ExpFusion, CommonInferShape);
static RegistryInferShape g_FakeQuantWithMinMaxVarsInferShape(mindspore::schema::PrimitiveType_FakeQuantWithMinMaxVars,
                                                              CommonInferShape);
static RegistryInferShape g_FloorInferShape(mindspore::schema::PrimitiveType_Floor, CommonInferShape);
static RegistryInferShape g_InstanceNormInferShape(mindspore::schema::PrimitiveType_InstanceNorm, CommonInferShape);
static RegistryInferShape g_L2NormInferShape(mindspore::schema::PrimitiveType_L2NormalizeFusion, CommonInferShape);
static RegistryInferShape g_LeakyReluInferShape(mindspore::schema::PrimitiveType_LeakyRelu, CommonInferShape);

static RegistryInferShape g_LocalResponseNormalizationInferShape(mindspore::schema::PrimitiveType_Lrn,
                                                                 CommonInferShape);

static RegistryInferShape g_LogGradInferShape(mindspore::schema::PrimitiveType_LogGrad, CommonInferShape);
static RegistryInferShape g_LogicalNotInferShape(mindspore::schema::PrimitiveType_LogicalNot, CommonInferShape);
static RegistryInferShape g_LrnInferShape(mindspore::schema::PrimitiveType_Lrn, CommonInferShape);
static RegistryInferShape g_NegInferShape(mindspore::schema::PrimitiveType_Neg, CommonInferShape);
static RegistryInferShape g_NegGradInferShape(mindspore::schema::PrimitiveType_NegGrad, CommonInferShape);
static RegistryInferShape g_PowerGradInferShape(mindspore::schema::PrimitiveType_PowerGrad, CommonInferShape);
static RegistryInferShape g_PReLUInferShape(mindspore::schema::PrimitiveType_PReLUFusion, CommonInferShape);
static RegistryInferShape g_ReverseInferShape(mindspore::schema::PrimitiveType_ReverseV2, CommonInferShape);
static RegistryInferShape g_RoundInferShape(mindspore::schema::PrimitiveType_Round, CommonInferShape);
static RegistryInferShape g_RsqrtInferShape(mindspore::schema::PrimitiveType_Rsqrt, CommonInferShape);
static RegistryInferShape g_ScaleInferShape(mindspore::schema::PrimitiveType_ScaleFusion, CommonInferShape);
static RegistryInferShape g_SqrtInferShape(mindspore::schema::PrimitiveType_Sqrt, CommonInferShape);
static RegistryInferShape g_SquareInferShape(mindspore::schema::PrimitiveType_Square, CommonInferShape);

static RegistryInferShape g_LshProjectionInferShape(mindspore::schema::PrimitiveType_LshProjection,
                                                    LshProjectionInferShape);
static RegistryInferShape g_SoftmaxCrossEntropyInferShape(
  mindspore::schema::PrimitiveType_SoftmaxCrossEntropyWithLogits, SoftmaxCrossEntropyInferShape);
static RegistryInferShape g_LogInferShape(mindspore::schema::PrimitiveType_Log, CommonInferShape);
static RegistryInferShape g_LessInferShape(mindspore::schema::PrimitiveType_Less, ArithmeticCompareInferShape);
static RegistryInferShape g_EqualInferShape(mindspore::schema::PrimitiveType_Equal, ArithmeticCompareInferShape);
static RegistryInferShape g_LessEqualInferShape(mindspore::schema::PrimitiveType_LessEqual,
                                                ArithmeticCompareInferShape);
static RegistryInferShape g_GreaterInferShape(mindspore::schema::PrimitiveType_Greater, ArithmeticCompareInferShape);
static RegistryInferShape g_GreaterEqualInferShape(mindspore::schema::PrimitiveType_GreaterEqual,
                                                   ArithmeticCompareInferShape);
static RegistryInferShape g_NotEqualInferShape(mindspore::schema::PrimitiveType_NotEqual, ArithmeticCompareInferShape);
static RegistryInferShape g_ShapeInferShape(mindspore::schema::PrimitiveType_Shape, ShapeInferShape);
static RegistryInferShape g_ReshapeInferShape(mindspore::schema::PrimitiveType_Reshape, ReshapeInferShape);
static RegistryInferShape g_OneHotInferShape(mindspore::schema::PrimitiveType_OneHot, OneHotInferShape);
static RegistryInferShape g_FftImagInferShape(mindspore::schema::PrimitiveType_FftImag, FftImagInferShape);
static RegistryInferShape g_LstmInferShape(mindspore::schema::PrimitiveType_LSTM, LstmInferShape);
static RegistryInferShape g_ReduceInferShape(mindspore::schema::PrimitiveType_ReduceFusion, ReduceInferShape);
static RegistryInferShape g_FlattenInferShape(mindspore::schema::PrimitiveType_Flatten, FlattenInferShape);
static RegistryInferShape g_CustomNormalizeInferShape(mindspore::schema::PrimitiveType_CustomNormalize,
                                                      CustomNormalizeInferShape);
static RegistryInferShape g_NonMaxSuppressionInferShape(mindspore::schema::PrimitiveType_NonMaxSuppression,
                                                        NonMaxSuppressionInferShape);
static RegistryInferShape g_CustomExtractFeaturesInferShape(mindspore::schema::PrimitiveType_CustomExtractFeatures,
                                                            CustomExtractFeaturesInferShape);
static RegistryInferShape g_ArgmaxInferShape(mindspore::schema::PrimitiveType_ArgMaxFusion, ArgmaxInferShape);
static RegistryInferShape g_UniqueInferShape(mindspore::schema::PrimitiveType_Unique, UniqueInferShape);

static RegistryInferShape g_TensorListFromTensorInferShape(mindspore::schema::PrimitiveType_TensorListFromTensor,
                                                           TensorListFromTensorInferShape);
static RegistryInferShape g_TensorListGetItemInferShape(mindspore::schema::PrimitiveType_TensorListGetItem,
                                                        TensorListGetItemInferShape);
static RegistryInferShape g_TensorListReserveInferShape(mindspore::schema::PrimitiveType_TensorListReserve,
                                                        TensorListReserveInferShape);
static RegistryInferShape g_TensorListSetItemInferShape(mindspore::schema::PrimitiveType_TensorListSetItem,
                                                        TensorListSetItemInferShape);
static RegistryInferShape g_TensorListStackInferShape(mindspore::schema::PrimitiveType_TensorListStack,
                                                      TensorListStackInferShape);
static RegistryInferShape g_PartialInferShape(mindspore::schema::PrimitiveType_PartialFusion, PartialInferShape);
static RegistryInferShape g_MergeInferShape(mindspore::schema::PrimitiveType_Merge, MergeInferShape);
static RegistryInferShape g_SwitchInferShape(mindspore::schema::PrimitiveType_Switch, SwitchInferShape);
static RegistryInferShape g_AssertOpInferShape(mindspore::schema::PrimitiveType_Assert, AssertOpInferShape);
static RegistryInferShape g_SparseSoftmaxCrossEntropyInferShape(
  mindspore::schema::PrimitiveType_SparseSoftmaxCrossEntropy, SparseSoftmaxCrossEntropyInferShape);
static RegistryInferShape g_DropoutInferShape(mindspore::schema::PrimitiveType_Dropout, DropoutInferShape);
static RegistryInferShape g_PriorBoxInferShape(mindspore::schema::PrimitiveType_PriorBox, PriorBoxInferShape);
static RegistryInferShape g_MinimumGradInferShape(mindspore::schema::PrimitiveType_MinimumGrad, MaximumGradInferShape);
static RegistryInferShape g_AdderInferShape(mindspore::schema::PrimitiveType_AdderFusion, Conv2dInferShape);
static RegistryInferShape g_ReciprocalInferShape(mindspore::schema::PrimitiveType_Reciprocal, CommonInferShape);
static RegistryInferShape g_SmoothL1LossInferShape(mindspore::schema::PrimitiveType_SmoothL1Loss, CommonInferShape);
static RegistryInferShape g_SmoothL1LossGradInferShape(mindspore::schema::PrimitiveType_SmoothL1LossGrad,
                                                       CommonInferShape);
static RegistryInferShape g_SigmoidCrossEntropyWithLogitsInferShape(
  mindspore::schema::PrimitiveType_SigmoidCrossEntropyWithLogits, CommonInferShape);
static RegistryInferShape g_SigmoidCrossEntropyWithLogitsGradInferShape(
  mindspore::schema::PrimitiveType_SigmoidCrossEntropyWithLogitsGrad, CommonInferShape);
static RegistryInferShape g_ModInferShape(mindspore::schema::PrimitiveType_Mod, ArithmeticInferShape);
static RegistryInferShape g_ControlDependInferShape(mindspore::schema::PrimitiveType_ControlDepend, CommonInferShape);
}  // namespace lite
}  // namespace mindspore
