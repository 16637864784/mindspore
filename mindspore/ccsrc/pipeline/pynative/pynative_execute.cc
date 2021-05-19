/**
 * Copyright 2019-2020 Huawei Technologies Co., Ltd
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

#include "pipeline/pynative/pynative_execute.h"

#include <typeinfo>
#include <map>
#include <set>
#include <memory>
#include <sstream>
#include <unordered_set>
#include <algorithm>

#include "debug/trace.h"
#include "pybind_api/ir/tensor_py.h"
#include "ir/param_info.h"
#include "ir/anf.h"
#include "ir/cell.h"
#include "ir/tensor.h"
#include "utils/any.h"
#include "utils/utils.h"
#include "utils/ms_context.h"
#include "utils/check_convert_utils.h"
#include "utils/context/context_extends.h"
#include "utils/config_manager.h"
#include "utils/convert_utils_py.h"
#include "frontend/operator/ops.h"
#include "frontend/operator/composite/do_signature.h"
#include "pipeline/jit/parse/data_converter.h"
#include "pipeline/jit/parse/parse_dynamic.h"
#include "pipeline/jit/parse/resolve.h"
#include "pipeline/jit/static_analysis/prim.h"
#include "pipeline/jit/static_analysis/auto_monad.h"
#include "pipeline/jit/pipeline.h"
#include "backend/session/session_factory.h"
#include "backend/optimizer/common/const_input_to_attr_registry.h"
#include "backend/optimizer/common/helper.h"
#include "pipeline/jit/action.h"

#include "pipeline/pynative/base.h"
#include "pybind_api/api_register.h"
#include "pybind_api/pybind_patch.h"
#include "vm/transform.h"

#include "frontend/optimizer/ad/grad.h"
#include "pipeline/jit/resource.h"
#include "pipeline/jit/pass.h"
#include "frontend/parallel/context.h"
#include "pipeline/jit/prim_bprop_optimizer.h"
#include "frontend/optimizer/ad/dfunctor.h"

#ifdef ENABLE_GE
#include "pipeline/pynative/pynative_execute_ge.h"
#endif

#include "debug/anf_ir_dump.h"

using mindspore::tensor::TensorPy;
const size_t PTR_LEN = 15;
// primitive unable to infer value for constant input in PyNative mode
static const std::set<std::string> vm_operators = {"make_ref", "HookBackward", "InsertGradientOf", "stop_gradient",
                                                   "mixed_precision_cast"};
static const char kOpsFunctionModelName[] = "mindspore.ops.functional";
static const char kMSDtypeModelName[] = "mindspore.common.dtype";
namespace mindspore::pynative {
static std::shared_ptr<session::SessionBasic> session = nullptr;
static std::shared_ptr<compile::MindRTBackend> mind_rt_backend = nullptr;
PynativeExecutorPtr PynativeExecutor::executor_ = nullptr;
ForwardExecutorPtr PynativeExecutor::forward_executor_ = nullptr;
GradExecutorPtr PynativeExecutor::grad_executor_ = nullptr;
std::mutex PynativeExecutor::instance_lock_;
constexpr auto implcast = "implcast";

template <typename T, typename... Args>
void PynativeExecutorTry(std::function<void(T *ret, const Args &...)> method, T *ret, const Args &... args) {
  const auto inst = PynativeExecutor::GetInstance();
  MS_EXCEPTION_IF_NULL(inst);
  MS_EXCEPTION_IF_NULL(method);
  try {
    method(ret, args...);
  } catch (const py::error_already_set &ex) {
    // print function call stack info before release
    std::ostringstream oss;
    trace::TraceGraphEval();
    trace::GetEvalStackInfo(oss);
    // call py::print to output function call stack to STDOUT, in case of output the log to file, the user can see
    // these info from screen, no need to open log file to find these info
    py::print(oss.str());
    MS_LOG(ERROR) << oss.str();
    inst->ClearRes();
    // re-throw this exception to Python interpreter to handle it
    throw(py::error_already_set(ex));
  } catch (const py::type_error &ex) {
    inst->ClearRes();
    throw py::type_error(ex);
  } catch (const py::value_error &ex) {
    inst->ClearRes();
    throw py::value_error(ex);
  } catch (const py::index_error &ex) {
    inst->ClearRes();
    throw py::index_error(ex);
  } catch (const py::name_error &ex) {
    inst->ClearRes();
    throw py::name_error(ex);
  } catch (const std::exception &ex) {
    inst->ClearRes();
    // re-throw this exception to Python interpreter to handle it
    throw(std::runtime_error(ex.what()));
  } catch (...) {
    inst->ClearRes();
    std::string exName(abi::__cxa_current_exception_type()->name());
    MS_LOG(EXCEPTION) << "Error occurred when compile graph. Exception name: " << exName;
  }
}

inline ValuePtr PyAttrValue(const py::object &obj) {
  ValuePtr converted_ret = parse::data_converter::PyDataToValue(obj);
  if (!converted_ret) {
    MS_LOG(EXCEPTION) << "Attribute convert error with type: " << std::string(py::str(obj));
  }
  return converted_ret;
}

static std::string GetId(const py::object &obj) {
  if (py::isinstance<tensor::Tensor>(obj)) {
    auto tensor_ptr = py::cast<tensor::TensorPtr>(obj);
    return tensor_ptr->id();
  } else if (py::isinstance<mindspore::Type>(obj)) {
    auto type_ptr = py::cast<mindspore::TypePtr>(obj);
    return "type" + type_ptr->ToString();
  } else if (py::isinstance<py::str>(obj) || py::isinstance<py::int_>(obj) || py::isinstance<py::float_>(obj)) {
    return std::string(py::str(obj));
  } else if (py::isinstance<py::none>(obj)) {
    return "none";
  } else if (py::isinstance<py::tuple>(obj) || py::isinstance<py::list>(obj)) {
    auto p_list = py::cast<py::tuple>(obj);
    string prefix = py::isinstance<py::tuple>(obj) ? "tuple:" : "list";
    if (p_list.empty()) {
      prefix = "empty";
    } else {
      std::string key;
      for (size_t i = 0; i < p_list.size(); ++i) {
        key += std::string(py::str(GetId(p_list[i]))) + ":";
      }
      prefix += key;
    }
    return prefix;
  }

  py::object ret = parse::python_adapter::CallPyFn(parse::PYTHON_MOD_PARSE_MODULE, parse::PYTHON_MOD_GET_OBJ_ID, obj);
  return py::cast<std::string>(ret);
}

std::map<SignatureEnumDType, std::vector<size_t>> GetTypeIndex(const std::vector<SignatureEnumDType> &dtypes) {
  std::map<SignatureEnumDType, std::vector<size_t>> type_indexes;
  for (size_t i = 0; i < dtypes.size(); ++i) {
    auto it = type_indexes.find(dtypes[i]);
    if (it == type_indexes.end()) {
      (void)type_indexes.emplace(std::make_pair(dtypes[i], std::vector<size_t>{i}));
    } else {
      it->second.emplace_back(i);
    }
  }
  return type_indexes;
}

TypeId JudgeMaxType(TypeId max_type, bool has_scalar_float32, bool has_scalar_int64, bool has_tensor_int8) {
  if (max_type == TypeId::kNumberTypeBool) {
    if (has_scalar_int64) {
      max_type = TypeId::kNumberTypeInt64;
    }
    if (has_scalar_float32) {
      max_type = TypeId::kNumberTypeFloat32;
    }
  }
  if (max_type != TypeId::kNumberTypeFloat16 && max_type != TypeId::kNumberTypeFloat32 &&
      max_type != TypeId::kNumberTypeFloat64 && max_type != TypeId::kTypeUnknown && has_scalar_float32) {
    max_type = TypeId::kNumberTypeFloat32;
  }
  if (max_type == TypeId::kNumberTypeUInt8 && has_tensor_int8) {
    max_type = TypeId::kNumberTypeInt16;
  }
  return max_type;
}

std::map<SignatureEnumDType, TypeId> GetDstType(const py::tuple &py_args,
                                                const std::map<SignatureEnumDType, std::vector<size_t>> &type_indexes) {
  std::map<SignatureEnumDType, TypeId> dst_type;
  for (auto it = type_indexes.begin(); it != type_indexes.end(); (void)++it) {
    auto type = it->first;
    auto indexes = it->second;
    if (type == SignatureEnumDType::kDTypeEmptyDefaultValue || indexes.size() < 2) {
      continue;
    }
    size_t priority = 0;
    TypeId max_type = TypeId::kTypeUnknown;
    bool has_scalar_float32 = false;
    bool has_scalar_int64 = false;
    bool has_tensor_int8 = false;
    for (size_t index : indexes) {
      auto obj = py_args[index];
      if (py::isinstance<py::float_>(obj)) {
        has_scalar_float32 = true;
      }
      if (!py::isinstance<py::bool_>(obj) && py::isinstance<py::int_>(obj)) {
        has_scalar_int64 = true;
      }
      if (py::isinstance<tensor::Tensor>(obj)) {
        auto arg = py::cast<tensor::TensorPtr>(obj);
        TypeId arg_type_id = arg->data_type();
        auto type_priority = prim::type_map.find(arg_type_id);
        if (type_priority == prim::type_map.end()) {
          continue;
        }
        if (arg_type_id == kNumberTypeInt8) {
          has_tensor_int8 = true;
        }
        if (type_priority->second > priority) {
          max_type = type_priority->first;
          priority = type_priority->second;
        }
      }
    }
    max_type = JudgeMaxType(max_type, has_scalar_float32, has_scalar_int64, has_tensor_int8);
    (void)dst_type.emplace(std::make_pair(type, max_type));
  }
  return dst_type;
}

std::string TypeIdToMsTypeStr(const TypeId &type_id) {
  auto type_name = type_name_map.find(type_id);
  if (type_name == type_name_map.end()) {
    MS_LOG(EXCEPTION) << "For implicit type conversion, not support convert to the type: " << TypeIdToType(type_id);
  }
  return type_name->second;
}

bool GetSignatureType(const PrimitivePyPtr &prim, std::vector<SignatureEnumDType> *dtypes) {
  MS_EXCEPTION_IF_NULL(dtypes);
  auto signature = prim->signatures();
  bool has_sig_dtype = false;
  (void)std::transform(signature.begin(), signature.end(), std::back_inserter(*dtypes),
                       [&has_sig_dtype](const Signature &sig) {
                         auto dtype = sig.dtype;
                         if (dtype != SignatureEnumDType::kDTypeEmptyDefaultValue) {
                           has_sig_dtype = true;
                         }
                         return dtype;
                       });
  return has_sig_dtype;
}

void PynativeInfer(const PrimitivePyPtr &prim, const py::list &py_args, OpExecInfo *const op_exec_info,
                   const abstract::AbstractBasePtrList &args_spec_list) {
  MS_LOG(DEBUG) << "Prim " << prim->name() << " input infer " << mindspore::ToString(args_spec_list);
  prim->BeginRecordAddAttr();
  AbstractBasePtr infer_res = EvalOnePrim(prim, args_spec_list)->abstract();
  prim->EndRecordAddAttr();
  op_exec_info->abstract = infer_res;
  MS_LOG(DEBUG) << "Prim " << prim->name() << " infer result " << op_exec_info->abstract->ToString();
}

std::string GetSingleOpGraphInfo(const OpExecInfoPtr &op_exec_info, const std::vector<tensor::TensorPtr> &input_tensors,
                                 const std::vector<int64_t> &tensors_mask) {
  MS_EXCEPTION_IF_NULL(op_exec_info);
  if (input_tensors.size() != tensors_mask.size()) {
    MS_LOG(EXCEPTION) << "Input tensors size " << input_tensors.size() << " should be equal to tensors mask size "
                      << tensors_mask.size();
  }
  std::string graph_info;
  // get input tensor info
  for (size_t index = 0; index < input_tensors.size(); ++index) {
    MS_EXCEPTION_IF_NULL(input_tensors[index]);
    auto tensor_shape = input_tensors[index]->shape();
    (void)std::for_each(tensor_shape.begin(), tensor_shape.end(), [&](const auto &dim) {
      (void)graph_info.append(std::to_string(dim));
      graph_info += "_";
    });
    (void)graph_info.append(std::to_string(input_tensors[index]->data_type()));
    graph_info += "_";
    (void)graph_info.append(input_tensors[index]->padding_type());
    graph_info += "_";
    auto tensor_addr = input_tensors[index]->device_address();
    if (tensor_addr != nullptr) {
      (void)graph_info.append(std::to_string(std::dynamic_pointer_cast<device::DeviceAddress>(tensor_addr)->type_id()));
      graph_info += "_";
      (void)graph_info.append(std::dynamic_pointer_cast<device::DeviceAddress>(tensor_addr)->format());
      graph_info += "_";
    }
    if (tensors_mask[index] == kValueNodeTensorMask) {
      if (input_tensors[index]->Dtype()->type_id() == kNumberTypeInt64) {
        (void)graph_info.append(std::to_string(*reinterpret_cast<int *>(input_tensors[index]->data_c())));
        graph_info += "_";
      } else if (input_tensors[index]->Dtype()->type_id() == kNumberTypeFloat32 ||
                 input_tensors[index]->Dtype()->type_id() == kNumberTypeFloat16) {
        (void)graph_info.append(std::to_string(*reinterpret_cast<float *>(input_tensors[index]->data_c())));
        graph_info += "_";
      } else {
        MS_LOG(EXCEPTION) << "The dtype of the constant input is not int64 or float32!";
      }
    }
  }
  // get prim and abstract info
  graph_info += (op_exec_info->op_name);
  graph_info += "_";
  // get attr info
  const auto &op_prim = op_exec_info->py_primitive;
  MS_EXCEPTION_IF_NULL(op_prim);
  const auto &attr_map = op_prim->attrs();
  (void)std::for_each(attr_map.begin(), attr_map.end(), [&](const auto &element) {
    graph_info += (element.second->ToString());
    graph_info += "_";
  });

  // Add output information(shape, type id) of the operator to graph_info to solve the problem of cache missing
  // caused by operators like DropoutGenMask whose output is related to values of input when input shapes are
  // the same but values are different
  auto abstr = op_exec_info->abstract;
  MS_EXCEPTION_IF_NULL(abstr);
  auto build_shape = abstr->BuildShape();
  MS_EXCEPTION_IF_NULL(build_shape);
  graph_info += (build_shape->ToString());
  graph_info += "_";
  auto build_type = abstr->BuildType();
  MS_EXCEPTION_IF_NULL(build_type);
  graph_info += std::to_string(build_type->type_id());
  graph_info += "_";

  return graph_info;
}

bool RunOpConvertConstInputToAttr(const py::object &input_object, size_t input_index, const PrimitivePtr &op_prim,
                                  const std::unordered_set<size_t> &input_attrs) {
  MS_EXCEPTION_IF_NULL(op_prim);
  auto input_names_value = op_prim->GetAttr(kAttrInputNames);
  if (input_names_value == nullptr) {
    return false;
  }
  auto input_names_vec = GetValue<std::vector<std::string>>(input_names_value);
  if (input_index >= input_names_vec.size()) {
    MS_LOG(EXCEPTION) << "The input index: " << input_index << " is large than the input names vector size!";
  }

  if (input_attrs.find(input_index) != input_attrs.end()) {
    ValuePtr value = parse::data_converter::PyDataToValue(input_object);
    MS_EXCEPTION_IF_NULL(value);
    auto input_name = input_names_vec[input_index];
    op_prim->AddAttr(input_name, value);
    return true;
  }
  return false;
}

void PlantTensorTupleToVector(const py::tuple &tuple_inputs, const PrimitivePtr &op_prim,
                              std::vector<tensor::TensorPtr> *input_tensors) {
  MS_EXCEPTION_IF_NULL(op_prim);
  MS_EXCEPTION_IF_NULL(input_tensors);
  for (const auto &input_object : tuple_inputs) {
    if (!py::isinstance<tensor::Tensor>(input_object)) {
      MS_LOG(EXCEPTION) << "The input object is not a tensor!";
    }
    auto tensor = py::cast<tensor::TensorPtr>(input_object);
    MS_EXCEPTION_IF_NULL(tensor);
    input_tensors->emplace_back(tensor);
  }
  op_prim->set_attr(kAttrDynInputSizes, MakeValue(std::vector<int64_t>{SizeToLong(tuple_inputs.size())}));
}

void ConvertValueTupleToTensor(const py::object &input_object, std::vector<tensor::TensorPtr> *input_tensors) {
  MS_EXCEPTION_IF_NULL(input_tensors);
  ValuePtr input_value = parse::data_converter::PyDataToValue(input_object);
  MS_EXCEPTION_IF_NULL(input_value);
  if (!input_value->isa<ValueTuple>()) {
    MS_LOG(EXCEPTION) << "The input object is not a value tuple!";
  }
  auto value_tuple = input_value->cast<ValueTuplePtr>();
  MS_EXCEPTION_IF_NULL(value_tuple);
  tensor::TensorPtr tensor_ptr = opt::CreateTupleTensor(value_tuple);
  MS_EXCEPTION_IF_NULL(tensor_ptr);
  input_tensors->emplace_back(tensor_ptr);
}

void ConvertMultiPyObjectToTensor(const py::object &input_object, const PrimitivePtr &op_prim,
                                  std::vector<tensor::TensorPtr> *input_tensors, int64_t *tensor_mask) {
  MS_EXCEPTION_IF_NULL(op_prim);
  MS_EXCEPTION_IF_NULL(input_tensors);
  MS_EXCEPTION_IF_NULL(tensor_mask);

  if (!py::isinstance<py::tuple>(input_object)) {
    MS_LOG(EXCEPTION) << "The input should be a tuple!";
  }
  auto tuple_inputs = py::cast<py::tuple>(input_object);
  if (tuple_inputs.empty()) {
    MS_LOG(EXCEPTION) << "The size of input list or tuple is 0!";
  }
  auto inputs = py::cast<py::tuple>(input_object);
  if (py::isinstance<tensor::Tensor>(inputs[0])) {
    PlantTensorTupleToVector(inputs, op_prim, input_tensors);
  } else {
    ConvertValueTupleToTensor(input_object, input_tensors);
    *tensor_mask = kValueNodeTensorMask;
  }
}

void ConvertPyObjectToTensor(const py::object &input_object, const PrimitivePtr &op_prim,
                             std::vector<tensor::TensorPtr> *input_tensors, int64_t *tensor_mask) {
  MS_EXCEPTION_IF_NULL(op_prim);
  MS_EXCEPTION_IF_NULL(input_tensors);
  MS_EXCEPTION_IF_NULL(tensor_mask);
  tensor::TensorPtr tensor_ptr = nullptr;
  if (py::isinstance<tensor::Tensor>(input_object)) {
    tensor_ptr = py::cast<tensor::TensorPtr>(input_object);
  } else if (py::isinstance<py::float_>(input_object)) {
    double input_value = py::cast<py::float_>(input_object);
    tensor_ptr = std::make_shared<tensor::Tensor>(input_value, kFloat32);
    *tensor_mask = kValueNodeTensorMask;
  } else if (py::isinstance<py::int_>(input_object)) {
    tensor_ptr = std::make_shared<tensor::Tensor>(py::cast<int64_t>(input_object), kInt64);
    *tensor_mask = kValueNodeTensorMask;
  } else if (py::isinstance<py::array>(input_object)) {
    tensor_ptr = TensorPy::MakeTensor(py::cast<py::array>(input_object), nullptr);
  } else if (py::isinstance<py::list>(input_object)) {
    auto list_inputs = py::cast<py::list>(input_object);
    py::tuple tuple_inputs(list_inputs.size());
    for (size_t i = 0; i < tuple_inputs.size(); ++i) {
      tuple_inputs[i] = list_inputs[i];
    }
    ConvertMultiPyObjectToTensor(tuple_inputs, op_prim, input_tensors, tensor_mask);
    return;
  } else if (py::isinstance<py::tuple>(input_object)) {
    ConvertMultiPyObjectToTensor(input_object, op_prim, input_tensors, tensor_mask);
    return;
  } else if (py::isinstance<py::none>(input_object)) {
    return;
  } else {
    MS_LOG(EXCEPTION) << "Run op inputs type is invalid!";
  }
  MS_EXCEPTION_IF_NULL(tensor_ptr);
  input_tensors->emplace_back(tensor_ptr);
}

void ConstructInputTensor(const OpExecInfoPtr &op_run_info, std::vector<int64_t> *tensors_mask,
                          std::vector<tensor::TensorPtr> *input_tensors) {
  MS_EXCEPTION_IF_NULL(op_run_info);
  MS_EXCEPTION_IF_NULL(tensors_mask);
  MS_EXCEPTION_IF_NULL(input_tensors);
  PrimitivePtr op_prim = op_run_info->py_primitive;
  MS_EXCEPTION_IF_NULL(op_prim);

  opt::ConstInputToAttrInfoRegister reg;
  bool reg_exist = opt::ConstInputToAttrInfoRegistry::Instance().GetRegisterByOpName(op_run_info->op_name, &reg);
  if (op_run_info->is_dynamic_shape &&
      dynamic_shape_const_input_to_attr.find(op_run_info->op_name) == dynamic_shape_const_input_to_attr.end()) {
    MS_LOG(INFO) << "current node is dynamic shape: " << op_run_info->op_name;
    reg_exist = false;
  }
  auto ms_context = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(ms_context);
  if (op_run_info->op_name == prim::kPrimEmbeddingLookup->name()) {
    if (ms_context->get_param<std::string>(MS_CTX_DEVICE_TARGET) != kCPUDevice) {
      reg_exist = false;
    }
  }
  if (op_run_info->op_name == prim::kPrimGatherD->name()) {
    // Gather op needs converting const input to attr on GPU device
    if (ms_context->get_param<std::string>(MS_CTX_DEVICE_TARGET) != kGPUDevice) {
      reg_exist = false;
    }
  }

  op_prim->BeginRecordAddAttr();
  size_t input_num = op_run_info->op_inputs.size();
  for (size_t index = 0; index < input_num; ++index) {
    // convert const input to attr
    if (reg_exist &&
        RunOpConvertConstInputToAttr(op_run_info->op_inputs[index], index, op_prim, reg.GetConstInputAttrInfo())) {
      continue;
    }
    // convert const and tuple input to tensor
    int64_t tensor_mask = static_cast<int64_t>(op_run_info->inputs_mask[index]);
    ConvertPyObjectToTensor(op_run_info->op_inputs[index], op_prim, input_tensors, &tensor_mask);
    // mark tensors, data : 0, weight : 1, valuenode: 2
    op_run_info->inputs_mask[index] = tensor_mask;
    std::vector<int64_t> new_mask(input_tensors->size() - tensors_mask->size(), tensor_mask);
    tensors_mask->insert(tensors_mask->end(), new_mask.begin(), new_mask.end());
  }
  op_prim->EndRecordAddAttr();
}

void ConvertAttrToUnifyMindIR(const OpExecInfoPtr &op_run_info) {
  MS_EXCEPTION_IF_NULL(op_run_info);
  PrimitivePtr op_prim = op_run_info->py_primitive;
  MS_EXCEPTION_IF_NULL(op_prim);

  std::string op_name = op_run_info->op_name;
  auto attrs = op_prim->attrs();
  for (auto attr : attrs) {
    bool converted = CheckAndConvertUtils::ConvertAttrValueToString(op_name, attr.first, &attr.second);
    if (converted) {
      op_prim->set_attr(attr.first, attr.second);
    }
    bool converted_ir_attr = CheckAndConvertUtils::CheckIrAttrtoOpAttr(op_name, attr.first, &attr.second);
    if (converted_ir_attr) {
      op_prim->set_attr(attr.first, attr.second);
    }
  }
}

BaseRef TransformBaseRefListToTuple(const BaseRef &base_ref) {
  if (utils::isa<VectorRef>(base_ref)) {
    auto ref_list = utils::cast<VectorRef>(base_ref);
    py::tuple output_tensors(ref_list.size());
    for (size_t i = 0; i < ref_list.size(); ++i) {
      auto output = TransformBaseRefListToTuple(ref_list[i]);
      if (utils::isa<tensor::TensorPtr>(output)) {
        auto tensor_ptr = utils::cast<tensor::TensorPtr>(output);
        MS_EXCEPTION_IF_NULL(tensor_ptr);
        output_tensors[i] = tensor_ptr;
      } else if (utils::isa<PyObjectRef>(output)) {
        py::object obj = utils::cast<PyObjectRef>(output).object_;
        py::tuple tensor_tuple = py::cast<py::tuple>(obj);
        output_tensors[i] = tensor_tuple;
      } else {
        MS_LOG(EXCEPTION) << "The output is not a base ref list or a tensor!";
      }
    }
    return std::make_shared<PyObjectRef>(output_tensors);
  } else if (utils::isa<tensor::TensorPtr>(base_ref)) {
    return base_ref;
  } else {
    MS_LOG(EXCEPTION) << "The output is not a base ref list or a tensor!";
  }
}

size_t GetTupleSize(const py::tuple &args) {
  size_t count = 0;
  for (size_t i = 0; i < args.size(); i++) {
    if (py::isinstance<py::tuple>(args[i])) {
      count += GetTupleSize(args[i]);
    } else {
      count += 1;
    }
  }
  return count;
}

void ConvertTupleArg(py::tuple *res, size_t *index, const py::tuple &arg) {
  for (size_t i = 0; i < arg.size(); i++) {
    if (py::isinstance<py::tuple>(arg[i])) {
      ConvertTupleArg(res, index, arg[i]);
    } else {
      (*res)[(*index)++] = arg[i];
    }
  }
}

py::tuple ConvertArgs(const py::tuple &args) {
  size_t tuple_size = GetTupleSize(args);
  py::tuple res(tuple_size);
  size_t index = 0;
  for (size_t i = 0; i < args.size(); i++) {
    if (py::isinstance<py::tuple>(args[i])) {
      ConvertTupleArg(&res, &index, args[i]);
    } else {
      res[index++] = args[i];
    }
  }
  return res;
}

void ResetTopCellInfo(const TopCellInfoPtr &top_cell, const py::args &args) {
  MS_EXCEPTION_IF_NULL(top_cell);
  top_cell->set_op_num(0);
  top_cell->set_all_op_info("");
  top_cell->set_forward_already_run(true);
  std::string input_args_id;
  for (size_t i = 0; i < args.size(); ++i) {
    input_args_id += GetId(args[i]) + "_";
  }
  top_cell->set_input_args_id(input_args_id);
}

void SaveOpInfo(const TopCellInfoPtr &top_cell, const std::string &op_info,
                const std::vector<tensor::TensorPtr> &op_out_tensors) {
  MS_EXCEPTION_IF_NULL(top_cell);
  auto &op_info_with_tensor_id = top_cell->op_info_with_tensor_id();
  if (op_info_with_tensor_id.find(op_info) != op_info_with_tensor_id.end()) {
    MS_LOG(EXCEPTION) << "Top cell: " << top_cell.get() << " records op info with tensor id, but get op info "
                      << op_info << " in op_info_with_tensor_id map";
  }
  // Record the relationship between the forward op and its output tensor id
  std::for_each(op_out_tensors.begin(), op_out_tensors.end(),
                [&](const tensor::TensorPtr &tensor) { op_info_with_tensor_id[op_info].emplace_back(tensor->id()); });
}

void UpdateTensorInfo(const tensor::TensorPtr &new_tensor, const std::vector<tensor::TensorPtr> &pre_tensors) {
  MS_EXCEPTION_IF_NULL(new_tensor);
  auto device_target = MsContext::GetInstance()->get_param<std::string>(MS_CTX_DEVICE_TARGET);
  for (auto &pre_tensor : pre_tensors) {
    MS_EXCEPTION_IF_NULL(pre_tensor);
    MS_LOG(DEBUG) << "Replace Old tensor " << pre_tensor.get() << " id " << pre_tensor->id()
                  << " device_address: " << pre_tensor->device_address()->GetMutablePtr() << " shape and type "
                  << pre_tensor->GetShapeAndDataTypeInfo() << " with New tensor " << new_tensor.get() << " id "
                  << new_tensor->id() << " device_address " << new_tensor->device_address()->GetMutablePtr()
                  << " shape and dtype " << new_tensor->GetShapeAndDataTypeInfo();
    pre_tensor->set_shape(new_tensor->shape());
    pre_tensor->set_data_type(new_tensor->data_type());
    if (device_target != kCPUDevice) {
      pre_tensor->set_device_address(new_tensor->device_address());
    } else {
      auto old_device_address = std::dynamic_pointer_cast<device::DeviceAddress>(pre_tensor->device_address());
      auto new_device_address = std::dynamic_pointer_cast<device::DeviceAddress>(new_tensor->device_address());
      auto old_ptr = old_device_address->GetMutablePtr();
      MS_EXCEPTION_IF_NULL(old_ptr);
      auto new_ptr = new_device_address->GetPtr();
      MS_EXCEPTION_IF_NULL(new_ptr);
      auto ret = memcpy_s(old_ptr, old_device_address->GetSize(), new_ptr, new_device_address->GetSize());
      if (ret != EOK) {
        MS_LOG(EXCEPTION) << "Memory copy failed. ret: " << ret;
      }
    }
  }
}

void ClearPyNativeSession() { session = nullptr; }

void CheckPyNativeContext() {
  auto parallel_context = parallel::ParallelContext::GetInstance();
  MS_EXCEPTION_IF_NULL(parallel_context);
  auto ms_context = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(ms_context);
  auto parallel_mode = parallel_context->parallel_mode();
  if (parallel_mode != parallel::STAND_ALONE && parallel_mode != parallel::DATA_PARALLEL &&
      ms_context->get_param<int>(MS_CTX_EXECUTION_MODE) == kPynativeMode) {
    MS_LOG(EXCEPTION) << "PyNative Only support STAND_ALONE and DATA_PARALLEL, but got:" << parallel_mode;
  }
}

py::object RunOp(const py::args &args) {
  CheckPyNativeContext();
  auto executor = PynativeExecutor::GetInstance();
  MS_EXCEPTION_IF_NULL(executor);
  OpExecInfoPtr op_exec_info = executor->forward_executor()->GenerateOpExecInfo(args);
  MS_EXCEPTION_IF_NULL(op_exec_info);
  MS_LOG(DEBUG) << "RunOp name: " << op_exec_info->op_name << " start, args: " << args.size();
  py::object ret = py::none();
  PynativeExecutorTry(executor->forward_executor()->RunOpS, &ret, op_exec_info);
  return ret;
}

GradExecutorPtr ForwardExecutor::grad() const {
  auto grad_executor = grad_executor_.lock();
  MS_EXCEPTION_IF_NULL(grad_executor);
  return grad_executor;
}

bool TopCellInfo::IsSubCell(const std::string &cell_id) const {
  if (sub_cell_list_.empty()) {
    MS_LOG(DEBUG) << "The sub cell list is empty, there is no sub cell";
    return false;
  }
  if (sub_cell_list_.find(cell_id) != sub_cell_list_.end()) {
    return true;
  }
  return false;
}

void TopCellInfo::clear() {
  MS_LOG(DEBUG) << "Clear top cell info. Cell id " << cell_id_;
  op_num_ = 0;
  is_dynamic_ = false;
  vm_compiled_ = false;
  ms_function_flag_ = false;
  is_init_kpynative_ = false;
  need_compile_graph_ = false;
  forward_already_run_ = false;
  input_args_id_.clear();
  all_op_info_.clear();

  if (resource_ != nullptr) {
    resource_->Clean();
    resource_ = nullptr;
  }
  df_builder_ = nullptr;
  k_pynative_cell_ptr_ = nullptr;
  graph_info_map_.clear();
  sub_cell_list_.clear();
  op_info_with_tensor_id_.clear();
  tensor_id_with_tensor_object_.clear();
}

void ForwardExecutor::RunOpInner(py::object *ret, const OpExecInfoPtr &op_exec_info) {
  MS_EXCEPTION_IF_NULL(ret);
  MS_EXCEPTION_IF_NULL(op_exec_info);
  if (op_exec_info->op_name == prim::kPrimMixedPrecisionCast->name()) {
    py::tuple res = RunOpWithInitBackendPolicy(op_exec_info);
    if (res.size() == 1) {
      *ret = res[0];
      return;
    }
    *ret = std::move(res);
    return;
  }
  // make cnode for building grad graph if grad flag is set.
  abstract::AbstractBasePtrList args_spec_list;
  std::vector<int64_t> op_masks;
  auto cnode = MakeCNode(op_exec_info, &op_masks, &args_spec_list);
  // Update op input mask and record op info
  op_exec_info->inputs_mask = op_masks;
  grad()->RecordGradOpInfo(op_exec_info);
  // get output abstract info
  bool is_find = false;
  GetOpOutputAbstract(op_exec_info, args_spec_list, &is_find);
  MS_LOG(DEBUG) << "Run op infer " << op_exec_info->op_name << " " << op_exec_info->abstract->ToString();
  *ret = GetOpOutputObject(op_exec_info, args_spec_list, cnode, is_find);
}

OpExecInfoPtr ForwardExecutor::GenerateOpExecInfo(const py::args &args) {
  if (args.size() != PY_ARGS_NUM) {
    MS_LOG(ERROR) << "Three args are needed by RunOp";
    return nullptr;
  }
  auto op_exec_info = std::make_shared<OpExecInfo>();
  auto op_name = py::cast<std::string>(args[PY_NAME]);
  op_exec_info->op_name = op_name;

  auto adapter = py::cast<PrimitivePyAdapterPtr>(args[PY_PRIM]);
  MS_EXCEPTION_IF_NULL(adapter);
  auto prim = adapter->attached_primitive();
  if (prim == nullptr) {
    prim = std::make_shared<PrimitivePy>(args[PY_PRIM], adapter);
    adapter->set_attached_primitive(prim);
  }

  if (!prim->HasPyObj()) {
    MS_LOG(EXCEPTION) << "Pyobj is empty";
  }
  op_exec_info->py_primitive = prim;
  op_exec_info->op_inputs = args[PY_INPUTS];
  return op_exec_info;
}

void ForwardExecutor::GetArgsSpec(const OpExecInfoPtr &op_exec_info, std::vector<int64_t> *op_masks,
                                  std::vector<AnfNodePtr> *inputs, abstract::AbstractBasePtrList *args_spec_list) {
  MS_EXCEPTION_IF_NULL(op_masks);
  auto prim = op_exec_info->py_primitive;
  for (size_t i = 0; i < op_exec_info->op_inputs.size(); i++) {
    abstract::AbstractBasePtr abs = nullptr;
    const auto &obj = op_exec_info->op_inputs[i];
    auto id = GetId(obj);
    auto it = node_abs_map_.find(id);
    if (it != node_abs_map_.end()) {
      abs = it->second;
    }

    bool op_mask = false;
    if (py::isinstance<tensor::MetaTensor>(obj)) {
      auto meta_tensor = obj.cast<tensor::MetaTensorPtr>();
      if (meta_tensor) {
        op_mask = meta_tensor->is_parameter();
      }
    }
    MS_LOG(DEBUG) << "Gen args i " << i << op_mask;
    (*op_masks).emplace_back(op_mask);

    // Construct grad graph
    if (grad()->need_construct_graph()) {
      AnfNodePtr input_node = nullptr;
      if (!grad()->top_cell_list().empty()) {
        bool requires_grad = true;
        if (op_mask) {
          auto requires_grad_attr = parse::python_adapter::GetPyObjAttr(obj, "requires_grad");
          requires_grad = requires_grad_attr.cast<bool>();
        }
        input_node = grad()->GetInput(obj, op_mask & requires_grad);
      }
      // update abstract
      if (input_node != nullptr) {
        if (input_node->abstract() != nullptr) {
          abs = input_node->abstract();
        }
        inputs->emplace_back(input_node);
      }
    }
    (*args_spec_list).emplace_back(CheckConstValue(prim, obj, abs, id, i));
  }
}

AnfNodePtr ForwardExecutor::MakeCNode(const OpExecInfoPtr &op_exec_info, std::vector<int64_t> *op_masks,
                                      abstract::AbstractBasePtrList *args_spec_list) {
  MS_EXCEPTION_IF_NULL(op_masks);
  MS_EXCEPTION_IF_NULL(args_spec_list);
  MS_EXCEPTION_IF_NULL(op_exec_info);

  auto prim = op_exec_info->py_primitive;

  const auto &signature = prim->signatures();
  auto sig_size = signature.size();
  auto size = op_exec_info->op_inputs.size();

  // ignore monad signature
  for (auto sig : signature) {
    if (sig.default_value != nullptr && sig.default_value->isa<Monad>()) {
      --sig_size;
    }
  }
  if (sig_size > 0 && sig_size != size) {
    MS_EXCEPTION(ValueError) << op_exec_info->op_name << " inputs size " << size << " does not match the requires "
                             << "inputs size " << sig_size;
  }
  if (op_exec_info->op_name != prim::kPrimCast->name()) {
    RunParameterAutoMixPrecisionCast(op_exec_info);
  }

  std::vector<AnfNodePtr> inputs;
  inputs.emplace_back(NewValueNode(prim));
  MS_LOG(DEBUG) << "Get op " << op_exec_info->op_name << " grad_flag " << grad()->grad_flag();
  GetArgsSpec(op_exec_info, op_masks, &inputs, args_spec_list);

  CNodePtr cnode = nullptr;
  if (grad()->need_construct_graph()) {
    cnode = grad()->curr_g()->NewCNodeInOrder(inputs);
    MS_LOG(DEBUG) << "Make CNode for " << op_exec_info->op_name << " new cnode is " << cnode->DebugString(4);
  }
  return cnode;
}

abstract::AbstractBasePtr ForwardExecutor::CheckConstValue(const PrimitivePyPtr &prim, const py::object &obj,
                                                           const abstract::AbstractBasePtr &abs, const std::string &id,
                                                           size_t index) {
  MS_EXCEPTION_IF_NULL(prim);
  auto const_input_index = prim->get_const_input_indexes();
  bool have_const_input = !const_input_index.empty();
  bool is_const_prim = prim->is_const_prim();
  auto new_abs = abs;
  MS_LOG(DEBUG) << prim->ToString() << " abs is nullptr " << (abs == nullptr) << " is_const_value "
                << prim->is_const_prim();
  bool is_const_input =
    have_const_input && std::find(const_input_index.begin(), const_input_index.end(), index) != const_input_index.end();
  if (abs == nullptr || is_const_prim || is_const_input) {
    MS_LOG(DEBUG) << "MakeCnode get node no in map " << id;
    ValuePtr input_value = PyAttrValue(obj);
    MS_EXCEPTION_IF_NULL(input_value);
    new_abs = input_value->ToAbstract();
    if (!is_const_prim && !is_const_input) {
      auto config = abstract::AbstractBase::kBroadenTensorOnly;
      MS_EXCEPTION_IF_NULL(new_abs);
      new_abs = new_abs->Broaden(config);
      MS_LOG(DEBUG) << "Broaden for " << prim->ToString() << " " << config;
      node_abs_map_[id] = new_abs;
    }
  }
  return new_abs;
}

void ForwardExecutor::GetOpOutputAbstract(const OpExecInfoPtr &op_exec_info,
                                          const abstract::AbstractBasePtrList &args_spec_list, bool *is_find) {
  MS_EXCEPTION_IF_NULL(is_find);
  MS_EXCEPTION_IF_NULL(op_exec_info);
  *is_find = false;
  auto op_name = op_exec_info->op_name;
  auto prim = op_exec_info->py_primitive;
  MS_EXCEPTION_IF_NULL(prim);

  auto temp = prim_abs_list_.find(prim->id());
  if (temp != prim_abs_list_.end()) {
    MS_LOG(DEBUG) << "Match prim input args " << op_name << mindspore::ToString(args_spec_list);
    auto iter = temp->second.find(args_spec_list);
    if (iter != temp->second.end()) {
      MS_LOG(DEBUG) << "Match prim ok " << op_name;
      op_exec_info->abstract = iter->second.abs;
      prim->set_evaluate_added_attrs(iter->second.attrs);
      *is_find = true;
    }
  }

  if (op_exec_info->abstract == nullptr || force_infer_prim.find(op_name) != force_infer_prim.end()) {
    // use python infer method
    if (ignore_infer_prim.find(op_name) == ignore_infer_prim.end()) {
      PynativeInfer(prim, op_exec_info->op_inputs, op_exec_info.get(), args_spec_list);
    }
  }
  // get output dynamic shape info
  auto abstract = op_exec_info->abstract;
  MS_EXCEPTION_IF_NULL(abstract);
  auto shape = abstract->BuildShape();
  MS_EXCEPTION_IF_NULL(shape);

  if (shape->IsDynamic()) {
    op_exec_info->is_dynamic_shape = true;
  }
}

py::object ForwardExecutor::GetOpOutputObject(const OpExecInfoPtr &op_exec_info,
                                              const abstract::AbstractBasePtrList &args_spec_list,
                                              const AnfNodePtr &CNode, bool out_abstract_existed) {
  MS_EXCEPTION_IF_NULL(op_exec_info);
  auto prim = op_exec_info->py_primitive;
  MS_EXCEPTION_IF_NULL(prim);
  // Infer output value by constant folding
  py::dict output = abstract::ConvertAbstractToPython(op_exec_info->abstract);
  if (!output["value"].is_none()) {
    return output["value"];
  }
  if (prim->is_const_prim()) {
    return py::cast("");
  }

  // Add output abstract info into cache, the const value needs to infer evert step
  if (!out_abstract_existed && !op_exec_info->is_dynamic_shape) {
    auto &out = prim_abs_list_[prim->id()];
    out[args_spec_list].abs = op_exec_info->abstract;
    out[args_spec_list].attrs = prim->evaluate_added_attrs();
  }

  // run op with selected backend
  auto result = RunOpWithInitBackendPolicy(op_exec_info);
  py::object out_real = result;
  if (result.size() == 1 && op_exec_info->abstract != nullptr &&
      !op_exec_info->abstract->isa<abstract::AbstractSequeue>()) {
    out_real = result[0];
  }

  // Save cnode info and build grad graph
  if (grad()->need_construct_graph() && !grad()->in_cell_with_custom_bprop_()) {
    MS_EXCEPTION_IF_NULL(CNode);
    std::string obj_id = GetId(out_real);
    CNode->set_abstract(op_exec_info->abstract);
    node_abs_map_[obj_id] = op_exec_info->abstract;
    grad()->SaveOutputNodeMap(obj_id, out_real, CNode);
    grad()->DoOpGrad(op_exec_info, CNode, out_real);
  } else {
    node_abs_map_.clear();
  }
  grad()->UpdateForwardTensorInfoInBpropGraph(op_exec_info, out_real);
  return out_real;
}

py::object ForwardExecutor::DoAutoCast(const py::object &arg, const TypeId &type_id, const std::string &op_name,
                                       size_t index, const std::string &obj_id) {
  py::tuple cast_args(3);
  cast_args[PY_PRIM] = parse::python_adapter::GetPyFn(kOpsFunctionModelName, "cast");
  cast_args[PY_NAME] = prim::kPrimCast->name();
  std::string dst_type_str = TypeIdToMsTypeStr(type_id);
  py::object dst_type = parse::python_adapter::GetPyFn(kMSDtypeModelName, dst_type_str);
  py::tuple inputs(2);
  inputs[0] = arg;
  inputs[1] = dst_type;
  cast_args[PY_INPUTS] = inputs;
  auto op_exec = GenerateOpExecInfo(cast_args);
  op_exec->is_mixed_precision_cast = true;
  op_exec->next_op_name = op_name;
  op_exec->next_input_index = index;
  // Cache the cast struct
  if (obj_id != implcast) {
    cast_struct_map_[obj_id] = op_exec;
  }
  py::object ret = py::none();
  RunOpInner(&ret, op_exec);
  return ret;
}

py::object ForwardExecutor::DoParamMixPrecisionCast(bool *is_cast, const py::object &obj, const std::string &op_name,
                                                    size_t index) {
  MS_EXCEPTION_IF_NULL(is_cast);
  auto tensor = py::cast<tensor::TensorPtr>(obj);
  auto cast_type = tensor->cast_dtype();
  py::object cast_output = obj;
  if (cast_type != nullptr) {
    auto source_element = tensor->Dtype();
    if (source_element != nullptr && IsSubType(source_element, kFloat) && *source_element != *cast_type) {
      MS_LOG(DEBUG) << "Cast to " << cast_type->ToString();
      *is_cast = true;
      // Get obj id
      auto id = GetId(obj);
      // Find obj id in unorder map
      auto cast_struct_pair = cast_struct_map_.find(id);
      if (cast_struct_pair != cast_struct_map_.end()) {
        // Update input for cast struct
        auto cast_struct = cast_struct_pair->second;
        cast_struct->op_inputs[0] = obj;
        py::object ret = py::none();
        RunOpInner(&ret, cast_struct);
        return ret;
      } else {
        return DoAutoCast(obj, cast_type->type_id(), op_name, index, id);
      }
    }
  }
  return cast_output;
}

py::object ForwardExecutor::DoParamMixPrecisionCastTuple(bool *is_cast, const py::tuple &tuple,
                                                         const std::string &op_name, size_t index) {
  MS_EXCEPTION_IF_NULL(is_cast);
  auto tuple_size = static_cast<int64_t>(tuple.size());
  py::tuple result(tuple_size);

  for (int64_t i = 0; i < tuple_size; i++) {
    if (py::isinstance<tensor::MetaTensor>(tuple[i])) {
      MS_LOG(DEBUG) << "Call cast for item " << i;
      result[i] = DoParamMixPrecisionCast(is_cast, tuple[i], op_name, index);
    } else if (py::isinstance<py::tuple>(tuple[i]) || py::isinstance<py::list>(tuple[i])) {
      result[i] = DoParamMixPrecisionCastTuple(is_cast, tuple[i], op_name, index);
    } else {
      result[i] = tuple[i];
    }
  }
  return std::move(result);
}

void ForwardExecutor::DoSignatrueCast(const PrimitivePyPtr &prim, const std::map<SignatureEnumDType, TypeId> &dst_type,
                                      const std::vector<SignatureEnumDType> &dtypes,
                                      const OpExecInfoPtr &op_exec_info) {
  const auto &signature = prim->signatures();
  auto &out_args = op_exec_info->op_inputs;
  for (size_t i = 0; i < out_args.size(); ++i) {
    // No need to implicit cast if no dtype.
    if (dtypes.empty() || dtypes[i] == SignatureEnumDType::kDTypeEmptyDefaultValue) {
      continue;
    }
    auto it = dst_type.find(dtypes[i]);
    if (it == dst_type.end() || it->second == kTypeUnknown) {
      continue;
    }
    MS_LOG(DEBUG) << "Check inputs " << i;
    auto obj = out_args[i];
    auto sig = SignatureEnumRW::kRWDefault;
    if (!signature.empty()) {
      sig = signature[i].rw;
    }
    bool is_parameter = false;
    TypeId arg_type_id = kTypeUnknown;
    if (py::isinstance<tensor::MetaTensor>(obj)) {
      auto arg = py::cast<tensor::MetaTensorPtr>(obj);
      if (arg->is_parameter()) {
        is_parameter = true;
        MS_LOG(DEBUG) << "Parameter is read " << i;
      }
      arg_type_id = arg->data_type();
    }
    // implicit cast
    bool is_same_type = false;
    if (arg_type_id != kTypeUnknown) {
      is_same_type = (prim::type_map.find(arg_type_id) == prim::type_map.end() || arg_type_id == it->second);
    }
    if (sig == SignatureEnumRW::kRWWrite) {
      if (!is_parameter) {
        prim::RaiseExceptionForCheckParameter(prim->name(), i, "not");
      }
      if (arg_type_id != kTypeUnknown) {
        if (!is_same_type) {
          prim::RaiseExceptionForConvertRefDtype(prim->name(), TypeIdToMsTypeStr(arg_type_id),
                                                 TypeIdToMsTypeStr(it->second));
        }
      }
    }
    if (is_same_type) {
      continue;
    }

    if (!py::isinstance<tensor::Tensor>(obj) && !py::isinstance<py::int_>(obj) && !py::isinstance<py::float_>(obj)) {
      MS_EXCEPTION(TypeError) << "For '" << prim->name() << "', the " << i
                              << "th input is a not support implicit conversion type: "
                              << py::cast<std::string>(obj.attr("__class__").attr("__name__")) << ", and the value is "
                              << py::cast<py::str>(obj) << ".";
    }
    py::object cast_output = DoAutoCast(out_args[i], it->second, op_exec_info->op_name, i, implcast);
    out_args[i] = cast_output;
  }
}

void ForwardExecutor::RunParameterAutoMixPrecisionCast(const OpExecInfoPtr &op_exec_info) {
  size_t size = op_exec_info->op_inputs.size();
  auto prim = op_exec_info->py_primitive;
  MS_EXCEPTION_IF_NULL(prim);
  const auto &signature = prim->signatures();
  for (size_t i = 0; i < size; i++) {
    auto obj = op_exec_info->op_inputs[i];
    auto sig = SignatureEnumRW::kRWDefault;
    if (!signature.empty()) {
      sig = signature[i].rw;
    }
    MS_LOG(DEBUG) << "Check mix precision " << op_exec_info->op_name << " input " << i << " "
                  << std::string(py::repr(obj));
    // mix precision for non param
    bool is_cast = false;
    py::object cast_output;
    if (py::isinstance<tensor::MetaTensor>(obj)) {
      auto meta_tensor = obj.cast<tensor::MetaTensorPtr>();
      if (meta_tensor && meta_tensor->is_parameter()) {
        if (sig != SignatureEnumRW::kRWRead) {
          continue;
        }
      }
      // redundant cast call if the tensor is a const Tensor.
      cast_output = DoParamMixPrecisionCast(&is_cast, obj, prim->name(), i);
    } else if (py::isinstance<py::tuple>(obj) || py::isinstance<py::list>(obj)) {
      // mix precision for tuple inputs
      cast_output = DoParamMixPrecisionCastTuple(&is_cast, obj, prim->name(), i);
    }
    if (is_cast) {
      op_exec_info->op_inputs[i] = cast_output;
    }
  }
  std::vector<SignatureEnumDType> dtypes;

  bool has_dtype_sig = GetSignatureType(prim, &dtypes);
  std::map<SignatureEnumDType, TypeId> dst_types;
  if (has_dtype_sig) {
    // fetch info for implicit cast
    auto type_indexes = GetTypeIndex(dtypes);
    dst_types = GetDstType(op_exec_info->op_inputs, type_indexes);
  }
  MS_LOG(DEBUG) << "Do signature for " << op_exec_info->op_name;
  DoSignatrueCast(prim, dst_types, dtypes, op_exec_info);
}

AnfNodePtr GradExecutor::GetInput(const py::object &obj, bool op_mask) {
  AnfNodePtr node = nullptr;
  std::string obj_id = GetId(obj);

  if (op_mask) {
    MS_LOG(DEBUG) << "Cell parameters(weights)";
    // get the parameter name from parameter object
    auto name_attr = parse::python_adapter::GetPyObjAttr(obj, "name");
    if (py::isinstance<py::none>(name_attr)) {
      MS_LOG(EXCEPTION) << "Parameter object should have name attribute";
    }
    auto param_name = py::cast<std::string>(name_attr);
    auto df_builder = GetDfbuilder(top_cell()->cell_id());
    MS_EXCEPTION_IF_NULL(df_builder);
    auto graph_info = top_cell()->graph_info_map().at(df_builder);
    MS_EXCEPTION_IF_NULL(graph_info);
    if (graph_info->params.find(obj_id) == graph_info->params.end()) {
      auto free_param = df_builder->add_parameter();
      free_param->set_name(param_name);
      free_param->debug_info()->set_name(param_name);
      auto value = py::cast<tensor::TensorPtr>(obj);
      free_param->set_default_param(value);
      MS_LOG(DEBUG) << "Top graph set free parameter " << obj_id;
      SetParamNodeMapInGraphInfoMap(df_builder, obj_id, free_param);
      SetParamNodeMapInGraphInfoMap(curr_g_, obj_id, free_param);
      SetNodeMapInGraphInfoMap(df_builder, obj_id, free_param);
      SetNodeMapInGraphInfoMap(curr_g_, obj_id, free_param);
      return free_param;
    }
    node = graph_info->params.at(obj_id);
    MS_EXCEPTION_IF_NULL(node);
    MS_LOG(DEBUG) << "Get input param node " << node->ToString() << " " << obj_id;
    return node;
  }

  auto graph_info = top_cell()->graph_info_map().at(curr_g_);
  MS_EXCEPTION_IF_NULL(graph_info);
  if (graph_info->node_map.find(obj_id) != graph_info->node_map.end()) {
    // op(x, y)
    // out = op(op1(x, y))
    // out = op(cell1(x, y))
    // out = op(cell1(x, y)[0])
    node = GetObjNode(obj, obj_id);
  } else if (py::isinstance<py::tuple>(obj) || py::isinstance<py::list>(obj)) {
    // out = op((x, y))
    // out = cell((x, y))
    auto tuple = obj.cast<py::tuple>();
    // cell((1,2)): support not mix (scalar, tensor)
    if (!tuple.empty() && !py::isinstance<tensor::Tensor>(tuple[0])) {
      return MakeValueNode(obj, obj_id);
    }
    std::vector<AnfNodePtr> args;
    args.emplace_back(NewValueNode(prim::kPrimMakeTuple));
    auto tuple_size = tuple.size();
    for (size_t i = 0; i < tuple_size; i++) {
      args.emplace_back(GetInput(tuple[i], false));
    }
    auto cnode = curr_g_->NewCNode(args);
    SetNodeMapInGraphInfoMap(curr_g_, GetId(obj), cnode);
    node = cnode;
  } else {
    node = MakeValueNode(obj, obj_id);
  }
  node == nullptr ? MS_LOG(DEBUG) << "Get node is nullptr"
                  : MS_LOG(DEBUG) << "Get input node " << node->ToString() << " " << obj_id;
  return node;
}
AnfNodePtr GradExecutor::GetObjNode(const py::object &obj, const std::string &obj_id) {
  auto graph_info = top_cell()->graph_info_map().at(curr_g_);
  MS_EXCEPTION_IF_NULL(graph_info);
  auto &out = graph_info->node_map.at(obj_id);
  if (out.second.size() == 1 && out.second[0] == -1) {
    return out.first;
  }
  MS_LOG(DEBUG) << "Output size " << out.second.size();

  // Params node
  if (graph_info->params.find(obj_id) != graph_info->params.end()) {
    auto para_node = out.first;
    for (auto &idx : out.second) {
      std::vector<AnfNodePtr> tuple_get_item_inputs{NewValueNode(prim::kPrimTupleGetItem), para_node,
                                                    NewValueNode(idx)};
      para_node = curr_g_->NewCNode(tuple_get_item_inputs);
    }
    return para_node;
  }

  // Normal node
  auto node = out.first->cast<CNodePtr>();
  auto abs = node->abstract();
  ValuePtr out_obj = nullptr;
  if (node->forward().first != nullptr) {
    out_obj = node->forward().first;
  } else {
    out_obj = PyAttrValue(obj);
  }
  for (auto &idx : out.second) {
    std::vector<AnfNodePtr> tuple_get_item_inputs{NewValueNode(prim::kPrimTupleGetItem), node, NewValueNode(idx)};
    node = curr_g_->NewCNode(tuple_get_item_inputs);
    if (out_obj->isa<ValueTuple>()) {
      node->add_input_value(out_obj, "");
      node->add_input_value(MakeValue(idx), "");
      out_obj = (*out_obj->cast<ValueTuplePtr>())[idx];
      node->set_forward(out_obj, "");
    }
    if (abs != nullptr && abs->isa<abstract::AbstractTuple>()) {
      auto prim_abs = dyn_cast<abstract::AbstractTuple>(abs)->elements()[idx];
      MS_LOG(DEBUG) << "Set tuple getitem abs " << prim_abs->ToString();
      node->set_abstract(prim_abs);
    }
  }
  if (node->abstract() != nullptr) {
    forward()->node_abs_map()[obj_id] = node->abstract();
  }
  MS_LOG(DEBUG) << "GetObjNode output " << node->DebugString(6);
  return node;
}

AnfNodePtr GradExecutor::MakeValueNode(const py::object &obj, const std::string &obj_id) {
  ValuePtr converted_ret = nullptr;
  parse::ConvertData(obj, &converted_ret);
  auto node = NewValueNode(converted_ret);
  SetNodeMapInGraphInfoMap(curr_g_, obj_id, node);
  return node;
}

TopCellInfoPtr GradExecutor::GetTopCell(const std::string &cell_id) const {
  for (const auto &top_cell : top_cell_list_) {
    MS_EXCEPTION_IF_NULL(top_cell);
    if (top_cell->cell_id() == cell_id) {
      return top_cell;
    }
  }
  return nullptr;
}

void GradExecutor::RecordGradOpInfo(const OpExecInfoPtr &op_exec_info) {
  // Record op info for judge whether the construct of cell has been changed
  if (!grad_flag()) {
    MS_LOG(DEBUG) << "grad flag is set to false, no need to record op info";
    return;
  }
  // Record input args info (weight or data)
  MS_EXCEPTION_IF_NULL(op_exec_info);
  std::string input_args_info;
  for (auto mask : op_exec_info->inputs_mask) {
    if (mask) {
      input_args_info += "w";
      continue;
    }
    input_args_info += "d";
  }
  // Record op name and index
  size_t curr_op_num = top_cell()->op_num();
  op_exec_info->op_info = op_exec_info->op_name + "-" + std::to_string(curr_op_num) + "-" + input_args_info;
  std::string curr_op_info = top_cell()->all_op_info() + "_" + op_exec_info->op_info;
  top_cell()->set_all_op_info(curr_op_info);
  top_cell()->set_op_num(curr_op_num + 1);
}

void GradExecutor::SaveOutputNodeMap(const std::string &obj_id, const py::object &out_real, const AnfNodePtr &cnode) {
  if (cell_stack_.empty()) {
    MS_LOG(DEBUG) << "No need save output";
    return;
  }
  MS_EXCEPTION_IF_NULL(cnode);
  MS_LOG(DEBUG) << "Cnode is " << cnode->DebugString(4) << " id " << obj_id;
  if (py::isinstance<py::tuple>(out_real)) {
    auto value = py::cast<py::tuple>(out_real);
    auto size = static_cast<int64_t>(value.size());
    if (size > 1) {
      for (int64_t i = 0; i < size; ++i) {
        auto value_id = GetId(value[i]);
        SetNodeMapInGraphInfoMap(curr_g_, value_id, cnode, i);
      }
    }
  }
  SetNodeMapInGraphInfoMap(curr_g_, obj_id, cnode);
}

// Run ad grad for curr op and connect grad graph with previous op
void GradExecutor::DoOpGrad(const OpExecInfoPtr &op_exec_info, const AnfNodePtr &node, const py::object &op_out) {
  if (grad_is_running_ && !bprop_grad_stack_.top()) {
    MS_LOG(DEBUG) << "Custom bprop, no need do op grad";
    return;
  }
  MS_EXCEPTION_IF_NULL(node);
  if (!node->isa<CNode>()) {
    MS_LOG(EXCEPTION) << "The node should be a cnode to run ad grad, but got node: " << node->ToString();
  }
  auto c_node = node->cast<CNodePtr>();
  MS_EXCEPTION_IF_NULL(c_node);

  // Get input values
  MS_EXCEPTION_IF_NULL(op_exec_info);
  ValuePtrList input_args;
  for (size_t i = 0; i < op_exec_info->op_inputs.size(); ++i) {
    auto arg = parse::data_converter::PyDataToValue(op_exec_info->op_inputs[i]);
    MS_EXCEPTION_IF_NULL(arg);
    input_args.emplace_back(arg);
  }
  // get output value
  auto out_value = parse::data_converter::PyDataToValue(op_out);
  MS_EXCEPTION_IF_NULL(out_value);

  if (!ad::GradPynativeOp(top_cell()->k_pynative_cell_ptr(), c_node, input_args, out_value)) {
    MS_LOG(EXCEPTION) << "Failed to run ad grad for op " << op_exec_info->op_name;
  }
}

void GradExecutor::MakeCNodeForMsFunction(const FuncGraphPtr &ms_func_graph, const py::args &args,
                                          const OpExecInfoPtr &op_exec_info, ValuePtrList *input_values,
                                          CNodePtr *ms_function_cnode) {
  MS_EXCEPTION_IF_NULL(op_exec_info);
  op_exec_info->op_inputs = args;
  // Get input node info of ms_function
  MS_EXCEPTION_IF_NULL(ms_func_graph);
  std::vector<AnfNodePtr> input_nodes{NewValueNode(ms_func_graph)};
  MS_EXCEPTION_IF_NULL(input_values);
  for (size_t i = 0; i < args.size(); ++i) {
    auto input_i_node = GetInput(args[i], false);
    MS_EXCEPTION_IF_NULL(input_i_node);
    MS_LOG(DEBUG) << "The input " << i << " node of ms_function graph is: " << input_i_node->DebugString();
    input_nodes.emplace_back(input_i_node);
    auto inp_i_value = parse::data_converter::PyDataToValue(args[i]);
    MS_EXCEPTION_IF_NULL(inp_i_value);
    MS_LOG(DEBUG) << "The input " << i << " value of ms_function graph is: " << inp_i_value->ToString();
    (*input_values).emplace_back(inp_i_value);
  }

  // Get weights info of ms_function
  auto df_builder = GetDfbuilder(top_cell()->cell_id());
  MS_EXCEPTION_IF_NULL(df_builder);
  for (const auto &anf_node : ms_func_graph->parameters()) {
    MS_EXCEPTION_IF_NULL(anf_node);
    auto param = anf_node->cast<ParameterPtr>();
    MS_EXCEPTION_IF_NULL(param);
    if (param->has_default()) {
      input_nodes.emplace_back(param);
      auto default_value = param->default_param();
      MS_EXCEPTION_IF_NULL(default_value);
      (*input_values).emplace_back(default_value);
      op_exec_info->op_inputs.append(default_value);
      // Add weights to df_builder
      SetParamNodeMapInGraphInfoMap(df_builder, param->name(), param);
      MS_LOG(DEBUG) << "Top graph set free parameter " << param->DebugString() << ". Its default value is "
                    << default_value->ToString() << ". Its name is: " << param->name();
    }
  }

  // Make a CNode which includes ms_function fprop graph and inputs node
  MS_EXCEPTION_IF_NULL(ms_function_cnode);
  *ms_function_cnode = curr_g_->NewCNode(input_nodes);
  MS_LOG(DEBUG) << "Make a nested cnode: " << (*ms_function_cnode)->DebugString(2);
}

// Make adjoint for ms_function fprop graph and connect it with previous op
void GradExecutor::MakeAdjointForMsFunction(const FuncGraphPtr &ms_func_graph, const FuncGraphPtr &fprop_g,
                                            const py::object &out, const py::args &args,
                                            const std::string &graph_phase) {
  ValuePtrList input_values;
  CNodePtr ms_function_cnode = nullptr;
  OpExecInfoPtr op_exec_info = std::make_shared<OpExecInfo>();
  MakeCNodeForMsFunction(ms_func_graph, args, op_exec_info, &input_values, &ms_function_cnode);
  MS_EXCEPTION_IF_NULL(ms_function_cnode);
  SetTupleArgsToGraphInfoMap(curr_g_, out, ms_function_cnode);
  SetNodeMapInGraphInfoMap(curr_g_, GetId(out), ms_function_cnode);
  // Record ms_function cnode info and update forward tensors
  op_exec_info->op_name = graph_phase;
  RecordGradOpInfo(op_exec_info);
  MS_LOG(DEBUG) << "Ms_function cnode op info: " << op_exec_info->op_info;
  UpdateForwardTensorInfoInBpropGraph(op_exec_info, out);
  // Add out and dout
  auto out_value = parse::data_converter::PyDataToValue(out);
  MS_EXCEPTION_IF_NULL(out_value);
  // Do ad grad for ms_function cnode
  auto k_pynative_cell_ptr = top_cell()->k_pynative_cell_ptr();
  MS_EXCEPTION_IF_NULL(k_pynative_cell_ptr);
  if (!k_pynative_cell_ptr->KPynativeWithFProp(ms_function_cnode, input_values, out_value, fprop_g)) {
    MS_LOG(EXCEPTION) << "Failed to make adjoint for ms_function cnode, ms_function cnode info: "
                      << ms_function_cnode->DebugString(2);
  }
  top_cell()->set_ms_function_flag(true);
}

void GradExecutor::UpdateForwardTensorInfoInBpropGraph(const OpExecInfoPtr &op_exec_info, const py::object &out_real) {
  if (!grad_flag()) {
    MS_LOG(DEBUG) << "The grad flag is false, no need to update forward op info in bprop graph";
    return;
  }
  MS_EXCEPTION_IF_NULL(top_cell_);
  MS_EXCEPTION_IF_NULL(op_exec_info);
  auto op_info = op_exec_info->op_info;
  MS_LOG(DEBUG) << "Current op info: " << op_info;

  std::vector<tensor::TensorPtr> all_op_tensors;
  // Get input tensors
  for (size_t i = 0; i < op_exec_info->op_inputs.size(); ++i) {
    TensorValueToTensor(parse::data_converter::PyDataToValue(op_exec_info->op_inputs[i]), &all_op_tensors);
  }
  // Get output tensors
  TensorValueToTensor(parse::data_converter::PyDataToValue(out_real), &all_op_tensors);
  // Save all tensors info of current op
  if (need_construct_graph()) {
    SaveOpInfo(top_cell_, op_info, all_op_tensors);
  }

  // First run top cell
  if (already_run_top_cell_.find(top_cell_->cell_id()) == already_run_top_cell_.end()) {
    MS_LOG(DEBUG) << "Top cell " << top_cell_->cell_id() << " run firstly";
    if (!need_construct_graph()) {
      MS_LOG(EXCEPTION) << "The cell stack is empty when running a new top cell " << top_cell_->cell_id();
    }
    return;
  }
  // Non-first run
  const auto &pre_top_cell = already_run_top_cell_.at(top_cell_->cell_id());
  MS_EXCEPTION_IF_NULL(pre_top_cell);
  if (pre_top_cell->op_info_with_tensor_id().find(op_info) == pre_top_cell->op_info_with_tensor_id().end()) {
    MS_LOG(DEBUG) << "Can not find op info " << op_info << " in op info with tensor id map. Top cell "
                  << top_cell_->cell_id();
    return;
  }

  // Update new output tensor info in bprop graph
  const auto &pre_op_tensor_id = pre_top_cell->op_info_with_tensor_id().at(op_info);
  if (pre_op_tensor_id.size() != all_op_tensors.size()) {
    MS_LOG(EXCEPTION) << "The size of pre op tensor id: " << pre_op_tensor_id.size()
                      << " is not equal to the size of all tensors of current op " << all_op_tensors.size();
  }
  const auto &pre_tensor_id_with_tensor_object = pre_top_cell->tensor_id_with_tensor_object();
  for (size_t i = 0; i < pre_op_tensor_id.size(); ++i) {
    auto pre_id = pre_op_tensor_id[i];
    if (pre_tensor_id_with_tensor_object.find(pre_id) == pre_tensor_id_with_tensor_object.end()) {
      continue;
    }
    const auto &new_tensor = all_op_tensors[i];
    const auto &pre_tensor_object = pre_tensor_id_with_tensor_object.at(pre_id);
    UpdateTensorInfo(new_tensor, pre_tensor_object);
  }
}

void GradExecutor::SaveForwardTensorInfoInBpropGraph(const ResourcePtr &resource) {
  MS_EXCEPTION_IF_NULL(resource);
  // Get all tensors id belong to forward op
  std::unordered_set<std::string> forward_op_tensor_id;
  const auto &op_info_with_tensor_id = top_cell()->op_info_with_tensor_id();
  for (const auto &elem : op_info_with_tensor_id) {
    std::for_each(elem.second.begin(), elem.second.end(),
                  [&](const std::string &tensor_id) { forward_op_tensor_id.emplace(tensor_id); });
  }
  auto &tensor_id_with_tensor_object_ = top_cell()->tensor_id_with_tensor_object();
  if (!tensor_id_with_tensor_object_.empty()) {
    MS_LOG(EXCEPTION) << "When compile a new graph, the map tensor_id_with_tensor_object should be empty. Top cell "
                      << top_cell()->cell_id();
  }
  const auto &bprop_graph = resource->func_graph();
  const auto &value_node_list = bprop_graph->value_nodes();
  std::vector<tensor::TensorPtr> tensors_in_bprop_graph;
  for (const auto &elem : value_node_list) {
    auto value_node = elem.first->cast<ValueNodePtr>();
    MS_EXCEPTION_IF_NULL(value_node);
    TensorValueToTensor(value_node->value(), &tensors_in_bprop_graph);
  }
  // Save tensor info in bprop graph
  for (const auto &tensor : tensors_in_bprop_graph) {
    if (tensor->device_address() == nullptr || forward_op_tensor_id.find(tensor->id()) == forward_op_tensor_id.end()) {
      continue;
    }
    tensor_id_with_tensor_object_[tensor->id()].emplace_back(tensor);
    MS_LOG(DEBUG) << "Save forward tensor " << tensor.get() << " id " << tensor->id()
                  << " device address: " << tensor->device_address()->GetMutablePtr() << " shape and dtype "
                  << tensor->GetShapeAndDataTypeInfo();
  }
}

py::tuple ForwardExecutor::RunOpWithInitBackendPolicy(const OpExecInfoPtr &op_exec_info) {
  auto backend_policy = InitEnv(op_exec_info);
  PynativeStatusCode status = PYNATIVE_UNKNOWN_STATE;
  // returns a null py::tuple on error
  py::object result = RunOpWithBackendPolicy(backend_policy, op_exec_info, &status);
  if (status != PYNATIVE_SUCCESS) {
    MS_LOG(EXCEPTION) << "Failed to run " << op_exec_info->op_name;
  }
  MS_LOG(DEBUG) << "RunOp end";
  return result;
}

MsBackendPolicy ForwardExecutor::InitEnv(const OpExecInfoPtr &op_exec_info) {
  MS_LOG(INFO) << "RunOp start, op name is: " << op_exec_info->op_name;
  parse::python_adapter::set_python_env_flag(true);
  MsBackendPolicy backend_policy;
#if (!defined ENABLE_GE)
  auto ms_context = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(ms_context);
  if (!context::IsTsdOpened(ms_context)) {
    if (!context::OpenTsd(ms_context)) {
      MS_LOG(EXCEPTION) << "Open tsd failed";
    }
  }
  if (ms_context->backend_policy() == "ms") {
    backend_policy = kMsBackendMsPrior;
  } else {
    backend_policy = kMsBackendVmOnly;
  }
#else
  auto ms_context = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(ms_context);
  context::PynativeInitGe(ms_context);
  backend_policy = kMsBackendGeOnly;
#endif
  if (vm_operators.find(op_exec_info->op_name) != vm_operators.end()) {
    backend_policy = kMsBackendVmOnly;
  }
  return backend_policy;
}

py::object ForwardExecutor::RunOpWithBackendPolicy(MsBackendPolicy backend_policy, const OpExecInfoPtr &op_exec_info,
                                                   PynativeStatusCode *status) {
  MS_EXCEPTION_IF_NULL(status);
  py::object result;
  switch (backend_policy) {
    case kMsBackendVmOnly: {
      // use vm only
      MS_LOG(INFO) << "RunOp use VM only backend";
      result = RunOpInVM(op_exec_info, status);
      break;
    }
    case kMsBackendGePrior: {
#ifdef ENABLE_GE
      // use GE first, use vm when GE fails
      MS_LOG(INFO) << "RunOp use GE first backend";
      result = RunOpInGE(op_exec_info, status);
      if (*status != PYNATIVE_SUCCESS) {
        result = RunOpInVM(op_exec_info, status);
      }
#endif
      break;
    }
    case kMsBackendMsPrior: {
      // use Ms first,use others when ms failed
      MS_LOG(INFO) << "RunOp use Ms first backend";
      result = RunOpInMs(op_exec_info, status);
      if (*status != PYNATIVE_SUCCESS) {
        MS_LOG(ERROR) << "RunOp use Ms backend failed!!!";
      }
      break;
    }
    default:
      MS_LOG(ERROR) << "No backend configured for run op";
  }
  return result;
}

py::object ForwardExecutor::RunOpInVM(const OpExecInfoPtr &op_exec_info, PynativeStatusCode *status) {
  MS_LOG(INFO) << "RunOpInVM start";
  MS_EXCEPTION_IF_NULL(status);
  MS_EXCEPTION_IF_NULL(op_exec_info);
  MS_EXCEPTION_IF_NULL(op_exec_info->py_primitive);

  auto &op_inputs = op_exec_info->op_inputs;
  if (op_exec_info->op_name == "HookBackward" || op_exec_info->op_name == "InsertGradientOf" ||
      op_exec_info->op_name == "stop_gradient") {
    py::tuple result(op_inputs.size());
    for (size_t i = 0; i < op_inputs.size(); i++) {
      py::object input = op_inputs[i];
      auto input_obj_id = GetId(input);
      auto tensor = py::cast<tensor::TensorPtr>(input);
      MS_EXCEPTION_IF_NULL(tensor);
      if (op_exec_info->op_name == "HookBackward") {
        // the input object is not a output of forward cnode, eg: parameter
        result[i] = tensor;
      } else {
        // the input object is a output of forward cnode
        auto new_tensor = std::make_shared<tensor::Tensor>(tensor->data_type(), tensor->shape(), tensor->data_ptr());
        new_tensor->set_device_address(tensor->device_address());
        new_tensor->set_sync_status(tensor->sync_status());
        result[i] = new_tensor;
      }
    }
    *status = PYNATIVE_SUCCESS;
    MS_LOG(INFO) << "RunOpInVM end";
    return std::move(result);
  }

  auto primitive = op_exec_info->py_primitive;
  MS_EXCEPTION_IF_NULL(primitive);
  auto result = primitive->RunPyComputeFunction(op_inputs);
  MS_LOG(INFO) << "RunOpInVM end";
  if (py::isinstance<py::none>(result)) {
    MS_LOG(ERROR) << "VM got the result none, please check whether it is failed to get func";
    *status = PYNATIVE_OP_NOT_IMPLEMENTED_ERR;
    py::tuple err_ret(0);
    return std::move(err_ret);
  }
  *status = PYNATIVE_SUCCESS;
  if (py::isinstance<py::tuple>(result)) {
    return result;
  }
  py::tuple tuple_result = py::make_tuple(result);
  return std::move(tuple_result);
}

py::object ForwardExecutor::RunOpInMs(const OpExecInfoPtr &op_exec_info, PynativeStatusCode *status) {
  MS_EXCEPTION_IF_NULL(op_exec_info);
  MS_EXCEPTION_IF_NULL(status);
  MS_LOG(INFO) << "Start run op [" << op_exec_info->op_name << "] with backend policy ms";
  auto ms_context = MsContext::GetInstance();
  ms_context->set_param<bool>(MS_CTX_ENABLE_PYNATIVE_INFER, true);

  if (session == nullptr) {
    std::string device_target = ms_context->get_param<std::string>(MS_CTX_DEVICE_TARGET);
    session = session::SessionFactory::Get().Create(device_target);
    MS_EXCEPTION_IF_NULL(session);
    session->Init(ms_context->get_param<uint32_t>(MS_CTX_DEVICE_ID));
  }

  std::vector<tensor::TensorPtr> input_tensors;
  std::vector<int64_t> tensors_mask;
  ConstructInputTensor(op_exec_info, &tensors_mask, &input_tensors);
  ConvertAttrToUnifyMindIR(op_exec_info);
  // get graph info for checking it whether existing in the cache
  std::string graph_info = GetSingleOpGraphInfo(op_exec_info, input_tensors, tensors_mask);
#if defined(__APPLE__)
  session::OpRunInfo op_run_info = {op_exec_info->op_name,
                                    op_exec_info->py_primitive,
                                    op_exec_info->abstract,
                                    op_exec_info->is_dynamic_shape,
                                    op_exec_info->is_mixed_precision_cast,
                                    op_exec_info->next_op_name,
                                    static_cast<int>(op_exec_info->next_input_index)};
#else
  session::OpRunInfo op_run_info = {op_exec_info->op_name,
                                    op_exec_info->py_primitive,
                                    op_exec_info->abstract,
                                    op_exec_info->is_dynamic_shape,
                                    op_exec_info->is_mixed_precision_cast,
                                    op_exec_info->next_op_name,
                                    op_exec_info->next_input_index};
#endif
  VectorRef outputs;
  if (!compile::IsMindRTUsed()) {
    session->RunOp(&op_run_info, graph_info, &input_tensors, &outputs, tensors_mask);
  } else {
    if (mind_rt_backend == nullptr) {
      std::string device_target = ms_context->get_param<std::string>(MS_CTX_DEVICE_TARGET);
      uint32_t device_id = ms_context->get_param<uint32_t>(MS_CTX_DEVICE_ID);
      mind_rt_backend = std::make_shared<compile::MindRTBackend>("ms", device_target, device_id);
    }
    const compile::ActorInfo &actor_info =
      mind_rt_backend->CompileGraph(op_run_info, graph_info, &tensors_mask, &input_tensors);
    outputs = mind_rt_backend->RunGraph(actor_info, &tensors_mask, &input_tensors);
  }

  if (op_exec_info->is_dynamic_shape) {
    op_exec_info->abstract = op_run_info.abstract;
  }
  auto result = BaseRefToPyData(outputs);
  ms_context->set_param<bool>(MS_CTX_ENABLE_PYNATIVE_INFER, false);
  *status = PYNATIVE_SUCCESS;
  MS_LOG(INFO) << "End run op [" << op_exec_info->op_name << "] with backend policy ms";
  return result;
}

void ForwardExecutor::ClearRes() {
  MS_LOG(DEBUG) << "Clear forward res";
  prim_abs_list_.clear();
  node_abs_map_.clear();
  cast_struct_map_.clear();
}

ForwardExecutorPtr GradExecutor::forward() const {
  auto forward_executor = forward_executor_.lock();
  MS_EXCEPTION_IF_NULL(forward_executor);
  return forward_executor;
}

TopCellInfoPtr GradExecutor::top_cell() const {
  MS_EXCEPTION_IF_NULL(top_cell_);
  return top_cell_;
}

FuncGraphPtr GradExecutor::curr_g() const {
  MS_EXCEPTION_IF_NULL(curr_g_);
  return curr_g_;
}

void GradExecutor::PushCellStack(const std::string &cell_id) { cell_stack_.push(cell_id); }

void GradExecutor::PopCellStack() {
  if (cell_stack_.empty()) {
    MS_LOG(EXCEPTION) << "Stack cell_statck_ is empty";
  }
  cell_stack_.pop();
}

void GradExecutor::PushHighOrderGraphStack(const TopCellInfoPtr &top_cell) {
  high_order_stack_.push(std::make_pair(curr_g_, top_cell));
}

TopCellInfoPtr GradExecutor::PopHighOrderGraphStack() {
  if (high_order_stack_.empty()) {
    MS_LOG(EXCEPTION) << "Stack high_order_stack_ is empty";
  }
  high_order_stack_.pop();
  TopCellInfoPtr top_cell = nullptr;
  if (!high_order_stack_.empty()) {
    auto t = high_order_stack_.top();
    curr_g_ = t.first;
    top_cell = t.second;
  }
  return top_cell;
}

std::string GradExecutor::GetCellId(const py::object &cell, const py::args &args) {
  auto cell_id = GetId(cell);
  for (size_t i = 0; i < args.size(); i++) {
    std::string arg_id = GetId(args[i]);
    auto it = forward()->node_abs_map().find(arg_id);
    if (it != forward()->node_abs_map().end()) {
      cell_id += "_" + it->second->BuildShape()->ToString();
      cell_id += it->second->BuildType()->ToString();
    } else {
      auto abs = PyAttrValue(args[i])->ToAbstract();
      auto config = abstract::AbstractBase::kBroadenTensorOnly;
      abs = abs->Broaden(config);
      forward()->node_abs_map()[arg_id] = abs;
      cell_id += "_" + abs->BuildShape()->ToString();
      cell_id += abs->BuildType()->ToString();
    }
  }
  return cell_id;
}

void GradExecutor::DumpGraphIR(const std::string &filename, const FuncGraphPtr &graph) {
  if (MsContext::GetInstance()->get_param<bool>(MS_CTX_SAVE_GRAPHS_FLAG)) {
    DumpIR(filename, graph);
  }
}

bool GradExecutor::IsNestedGrad() const {
  MS_LOG(DEBUG) << "Grad nested order is " << grad_order_;
  return grad_order_ > 1;
}

bool GradExecutor::IsCellObjIdEq(const std::string &l_cell_id, const std::string &r_cell_id) {
  // get end pos of obj_id
  size_t obj_id_end_idx = l_cell_id.find('_');
  if (obj_id_end_idx == std::string::npos) {
    obj_id_end_idx = l_cell_id.length();
  }
  // just compare obj_id, ignore args id
  int cmp_ret = l_cell_id.compare(0, obj_id_end_idx, r_cell_id, 0, obj_id_end_idx);
  return cmp_ret == 0;
}

bool GradExecutor::IsTopGraph(const std::string &cell_id) {
  auto &top_cell_id = top_cell()->cell_id();
  return IsCellObjIdEq(cell_id, top_cell_id);
}

bool GradExecutor::IsBpropGraph(const std::string &cell_id) {
  if (top_cell_ == nullptr) {
    return false;
  }
  return std::any_of(bprop_cell_list_.begin(), bprop_cell_list_.end(),
                     [&cell_id](const std::string &value) { return cell_id.find(value) != std::string::npos; });
}

void GradExecutor::UpdateTopCellInfo(bool forward_already_run, bool need_compile_graph, bool vm_compiled) {
  top_cell()->set_vm_compiled(vm_compiled);
  top_cell()->set_need_compile_graph(need_compile_graph);
  top_cell()->set_forward_already_run(forward_already_run);
}

void GradExecutor::ClearCellRes(const std::string &cell_id) {
  static bool clear_all_cell_res = false;
  // Grad clean
  if (cell_id.empty()) {
    MS_LOG(DEBUG) << "Clear all cell resources";
    clear_all_cell_res = true;
    for (const auto &it : top_cell_list_) {
      it->clear();
    }
    top_cell_list_.clear();
    already_run_top_cell_.clear();
    clear_all_cell_res = false;
    return;
  }
  if (clear_all_cell_res) {
    MS_LOG(DEBUG) << "In process of clearing all cell resources, so no need to clear single cell resource again";
    return;
  }
  // clear when cell destruction
  for (auto it = top_cell_list_.begin(); it != top_cell_list_.end();) {
    auto top_cell_id = (*it)->cell_id();
    if (IsCellObjIdEq(cell_id, top_cell_id)) {
      (*it)->clear();
      it = top_cell_list_.erase(it);
      if (already_run_top_cell_.find(top_cell_id) != already_run_top_cell_.end()) {
        (void)already_run_top_cell_.erase(top_cell_id);
      }
      MS_LOG(DEBUG) << "Clear top cell resource. Top cell id " << top_cell_id;
      continue;
    }
    it++;
  }
}

FuncGraphPtr GradExecutor::GetDfbuilder(const std::string &cell_id) {
  // If top graph hold
  for (auto it = top_cell_list_.rbegin(); it != top_cell_list_.rend(); ++it) {
    if (cell_id.find((*it)->cell_id()) != std::string::npos) {
      return (*it)->df_builder();
    }
  }
  // Current cell is not top graph, get first top cell
  if (!top_cell_list_.empty()) {
    return top_cell_list_.front()->df_builder();
  }
  return nullptr;
}

ResourcePtr GradExecutor::GetResource(const std::string &cell_id) {
  // If top graph hold
  for (auto it = top_cell_list_.rbegin(); it != top_cell_list_.rend(); ++it) {
    if (cell_id.find((*it)->cell_id()) != std::string::npos) {
      return (*it)->resource();
    }
  }
  // Current cell is not top graph, get first top cell
  if (!top_cell_list_.empty()) {
    return top_cell_list_.front()->resource();
  }
  return nullptr;
}

void GradExecutor::InitResourceAndDfBuilder(const std::string &cell_id, const py::args &args) {
  auto bprop_fn = [&]() {
    if (IsBpropGraph(cell_id)) {
      MS_LOG(DEBUG) << "Run bprop cell";
      curr_g_ = std::make_shared<FuncGraph>();
      auto graph_info_cg = std::make_shared<GraphInfo>(cell_id);
      top_cell()->graph_info_map()[curr_g_] = graph_info_cg;
      for (size_t i = 0; i < args.size(); ++i) {
        auto param = args[i];
        auto new_param = curr_g_->add_parameter();
        std::string param_id = GetId(param);
        SetTupleArgsToGraphInfoMap(curr_g_, param, new_param, true);
        SetNodeMapInGraphInfoMap(curr_g_, param_id, new_param);
        SetParamNodeMapInGraphInfoMap(curr_g_, param_id, new_param);
      }
      bprop_grad_stack_.push(false);
    } else if (top_cell()->grad_order() != grad_order_) {
      MakeNewTopGraph(cell_id, args, true);
      bprop_grad_stack_.push(true);
    }
  };

  if (cell_stack_.empty()) {
    if (grad_is_running_) {
      bprop_fn();
    } else {
      MakeNewTopGraph(cell_id, args, true);
    }
  } else {
    // High order
    if (IsNestedGrad()) {
      if (grad_is_running_) {
        bprop_fn();
      } else if (top_cell()->grad_order() != grad_order_) {
        MS_LOG(DEBUG) << "Enter nested graph";
        auto cur_top_is_dynamic = top_cell()->is_dynamic();
        MakeNewTopGraph(cell_id, args, false);
        // If outer is dynamic, inner set dynamic too
        if (cur_top_is_dynamic) {
          top_cell()->set_is_dynamic(cur_top_is_dynamic);
        }
      }
    }
  }
  PushCellStack(cell_id);
  // Init kPynativeCellPtr with input parameters of top cell
  if (!top_cell()->is_init_kpynative()) {
    auto graph_info_cg = std::make_shared<GraphInfo>(cell_id);
    top_cell()->graph_info_map()[curr_g_] = graph_info_cg;
    auto df_builder = GetDfbuilder(cell_id);
    auto graph_info_df = std::make_shared<GraphInfo>(cell_id);
    top_cell()->graph_info_map()[df_builder] = graph_info_df;
    // Init parameter info for make cnode and curr_g
    std::vector<ValuePtr> input_param_values;
    for (size_t i = 0; i < args.size(); ++i) {
      auto new_param = curr_g_->add_parameter();
      auto param_i = args[i];
      ValuePtr param_i_value = PyAttrValue(param_i);
      MS_EXCEPTION_IF_NULL(param_i_value);
      input_param_values.emplace_back(param_i_value);
      auto param_i_abs = param_i_value->ToAbstract();
      MS_EXCEPTION_IF_NULL(param_i_abs);
      new_param->set_abstract(param_i_abs->Broaden());
      std::string param_i_id = GetId(param_i);
      SetTupleArgsToGraphInfoMap(curr_g_, param_i, new_param, true);
      SetNodeMapInGraphInfoMap(curr_g_, param_i_id, new_param);
      SetParamNodeMapInGraphInfoMap(curr_g_, param_i_id, new_param);
    }
    top_cell()->set_k_pynative_cell_ptr(ad::GradPynativeCellBegin(curr_g_->parameters(), input_param_values));
    top_cell()->set_need_compile_graph(true);
    top_cell()->set_init_kpynative(true);
  } else {
    // Non-top cell
    top_cell()->sub_cell_list().emplace(cell_id);
  }
}

void GradExecutor::NewGraphInner(py::object *ret, const py::object &cell, const py::args &args) {
  auto cell_id = GetCellId(cell, args);
  MS_LOG(DEBUG) << "NewGraphInner start " << args.size() << " " << cell_id;
  // When the cell has custom bprop, in_custom_bprop_cell is lager than 0
  if (py::hasattr(cell, parse::CUSTOM_BPROP_NAME)) {
    custom_bprop_cell_count_ += 1;
  }
  if (top_cell_ != nullptr && cell_stack_.empty()) {
    // non-first step
    if (already_run_top_cell_.find(cell_id) != already_run_top_cell_.end()) {
      // top cell
      const auto &pre_top_cell = already_run_top_cell_.at(cell_id);
      if (!pre_top_cell->is_dynamic()) {
        MS_LOG(DEBUG) << "Top cell " << cell_id << " is not dynamic or ms_function, no need to run NewGraphInner again";
        ResetTopCellInfo(pre_top_cell, args);
        set_top_cell(pre_top_cell);
        return;
      }
    } else if (top_cell()->IsSubCell(cell_id) && !top_cell()->is_dynamic()) {
      // non-top cell
      MS_LOG(DEBUG) << "no need to run NewGraphInner again";
      return;
    }
  }
  // Init resource for resource and df_builder
  InitResourceAndDfBuilder(cell_id, args);
  // Check whether cell has dynamic construct
  if (!top_cell()->is_dynamic()) {
    bool is_dynamic = parse::DynamicParser::IsDynamicCell(cell);
    MS_LOG(DEBUG) << "Current cell dynamic " << is_dynamic;
    if (is_dynamic) {
      top_cell()->set_is_dynamic(is_dynamic);
    }
  }
}

void GradExecutor::MakeNewTopGraph(const string &cell_id, const py::args &args, bool is_topest) {
  for (const auto &arg : args) {
    if (py::isinstance<tensor::Tensor>(arg)) {
      auto tensor = arg.cast<tensor::TensorPtr>();
      if (tensor && tensor->is_parameter()) {
        MS_EXCEPTION(TypeError) << "The inputs could not be Parameter.";
      }
    }
  }

  std::string input_args_id;
  for (size_t i = 0; i < args.size(); ++i) {
    input_args_id += GetId(args[i]) + "_";
  }

  if (top_cell_list_.empty() && grad_order_ == 0) {
    ++grad_order_;
  }
  curr_g_ = std::make_shared<FuncGraph>();
  // Init resource for new top cell
  auto df_builder = std::make_shared<FuncGraph>();
  auto resource = std::make_shared<pipeline::Resource>();
  auto top_cell = std::make_shared<TopCellInfo>(is_topest, grad_order_, resource, df_builder, cell_id);
  top_cell->set_forward_already_run(true);
  top_cell->set_input_args_id(input_args_id);
  top_cell_list_.emplace_back(top_cell);
  PushHighOrderGraphStack(top_cell);
  set_top_cell(top_cell);
  MS_LOG(DEBUG) << "New top graph, df_builder ptr " << df_builder.get() << " resource ptr " << resource.get();
}

void GradExecutor::SetTupleArgsToGraphInfoMap(const FuncGraphPtr &g, const py::object &args, const AnfNodePtr &node,
                                              bool is_param) {
  if (!py::isinstance<py::tuple>(args) && !py::isinstance<py::list>(args)) {
    return;
  }
  auto tuple = args.cast<py::tuple>();
  auto tuple_size = static_cast<int64_t>(tuple.size());
  for (int64_t i = 0; i < tuple_size; ++i) {
    auto id = GetId(tuple[i]);
    if (is_param && node->isa<Parameter>()) {
      auto param = node->cast<ParameterPtr>();
      MS_EXCEPTION_IF_NULL(param);
      SetParamNodeMapInGraphInfoMap(g, id, param);
    }
    SetNodeMapInGraphInfoMap(g, id, node, i);
    SetTupleItemArgsToGraphInfoMap(g, tuple[i], node, std::vector<int64_t>{i}, is_param);
  }
}

void GradExecutor::SetTupleItemArgsToGraphInfoMap(const FuncGraphPtr &g, const py::object &args, const AnfNodePtr &node,
                                                  const std::vector<int64_t> &index_sequence, bool is_param) {
  if (!py::isinstance<py::tuple>(args) && !py::isinstance<py::list>(args)) {
    return;
  }
  auto tuple = args.cast<py::tuple>();
  auto tuple_size = static_cast<int64_t>(tuple.size());
  for (int64_t i = 0; i < tuple_size; ++i) {
    std::vector<int64_t> tmp = index_sequence;
    tmp.emplace_back(i);
    auto id = GetId(tuple[i]);
    if (is_param && node->isa<Parameter>()) {
      auto param = node->cast<ParameterPtr>();
      MS_EXCEPTION_IF_NULL(param);
      SetParamNodeMapInGraphInfoMap(g, id, param);
    }
    SetNodeMapInGraphInfoMap(g, id, node, tmp);
    SetTupleItemArgsToGraphInfoMap(g, tuple[i], node, tmp, is_param);
  }
}

void GradExecutor::CreateMakeTupleNodeForMultiOut(const std::string &cell_id, const FuncGraphPtr &curr_g,
                                                  const py::object &out) {
  MS_EXCEPTION_IF_NULL(curr_g);
  if (!(py::isinstance<py::tuple>(out) || py::isinstance<py::list>(out))) {
    MS_LOG(EXCEPTION) << "The out of top cell should be tuple or list when set maketuple as output node";
  }
  auto out_tuple = out.cast<py::tuple>();
  auto out_tuple_size = static_cast<int64_t>(out_tuple.size());

  // get input node and value
  std::vector<AnfNodePtr> inputs{NewValueNode(prim::kPrimMakeTuple)};
  ValuePtrList input_args;
  for (int64_t i = 0; i < out_tuple_size; i++) {
    inputs.emplace_back(GetInput(out_tuple[i], false));
    input_args.emplace_back(parse::data_converter::PyDataToValue(out_tuple[i]));
  }
  auto cnode = curr_g_->NewCNode(inputs);
  MS_LOG(DEBUG) << "Tuple output node info " << cnode->DebugString();
  // record node info in graph map
  auto out_id = GetId(out);
  SetTupleArgsToGraphInfoMap(curr_g_, out, cnode);
  SetNodeMapInGraphInfoMap(curr_g_, out_id, cnode);
  if (grad_is_running_ && !bprop_grad_stack_.top()) {
    MS_LOG(DEBUG) << "Custom bprop, no need GradPynativeOp";
    return;
  }
  // run ad for maketuple node
  ValuePtr out_value = parse::data_converter::PyDataToValue(out);
  ad::GradPynativeOp(top_cell()->k_pynative_cell_ptr(), cnode, input_args, out_value);
}

void GradExecutor::EndGraphInner(py::object *ret, const py::object &cell, const py::object &out, const py::args &args) {
  const auto &cell_id = GetCellId(cell, args);
  MS_LOG(DEBUG) << "EndGraphInner start " << args.size() << " " << cell_id;
  if (cell_stack_.empty()) {
    MS_LOG(DEBUG) << "Current cell " << cell_id << " no need to run EndGraphInner again";
    if (top_cell()->is_topest() && cell_id == top_cell()->cell_id()) {
      set_grad_flag(false);
    }
    return;
  }
  // Make output node in this case: x = op1, y = op2, return (x, y)
  auto out_id = GetId(out);
  auto graph_info = top_cell()->graph_info_map().at(curr_g_);
  MS_EXCEPTION_IF_NULL(graph_info);
  if (graph_info->node_map.find(out_id) == graph_info->node_map.end()) {
    if (py::isinstance<py::tuple>(out) || py::isinstance<py::list>(out)) {
      CreateMakeTupleNodeForMultiOut(cell_id, curr_g_, out);
    } else {
      MS_LOG(DEBUG) << "Set ValueNode as output for graph, out id: " << out_id;
      MakeValueNode(out, out_id);
    }
  }

  DoGradForCustomBprop(cell, out, args);
  PopCellStack();
  auto set_fg_fn = [&]() {
    AnfNodePtr output_node = GetObjNode(out, out_id);
    MS_EXCEPTION_IF_NULL(output_node);
    curr_g_->set_output(output_node);
  };
  if (grad_is_running_) {
    if (!bprop_grad_stack_.top()) {
      bprop_grad_stack_.pop();
      set_fg_fn();
      return;
    }
    bprop_grad_stack_.pop();
  }
  if (MsContext::GetInstance()->get_param<bool>(MS_CTX_SAVE_GRAPHS_FLAG)) {
    set_fg_fn();
    DumpIR("fg.ir", curr_g_);
  }
  // Reset grad flag and update output node of top cell
  if (cell_stack_.empty() && cell_id == top_cell()->cell_id()) {
    MS_LOG(DEBUG) << "Cur top last cell " << cell_id;
    set_grad_flag(false);
    // Update real output node of top cell for generating bprop graph
    AnfNodePtr output_node = GetObjNode(out, out_id);
    MS_EXCEPTION_IF_NULL(output_node);
    auto k_pynative_cell_ptr = top_cell()->k_pynative_cell_ptr();
    MS_EXCEPTION_IF_NULL(k_pynative_cell_ptr);
    k_pynative_cell_ptr->UpdateOutputNodeOfTopCell(output_node);
  }
  // Checkout whether need to compile graph when top cell has ran finished
  if (cell_id == top_cell()->cell_id()) {
    CheckNeedCompileGraph();
  }
}

void GradExecutor::DoGradForCustomBprop(const py::object &cell, const py::object &out, const py::args &args) {
  if (!py::hasattr(cell, parse::CUSTOM_BPROP_NAME)) {
    return;
  }
  custom_bprop_cell_count_ -= 1;
  if (custom_bprop_cell_count_ != 0) {
    return;
  }
  size_t par_number = py::tuple(parse::python_adapter::CallPyObjMethod(cell, "get_parameters")).size();
  if (par_number > 0) {
    MS_LOG(EXCEPTION) << "When user defines the net bprop, there are " << par_number
                      << " parameters that is not supported in the net.";
  }
  py::function bprop_func = py::getattr(cell, parse::CUSTOM_BPROP_NAME);
  auto bprop_func_cellid = GetId(bprop_func);
  bprop_cell_list_.emplace_back(bprop_func_cellid);
  auto fake_prim = std::make_shared<PrimitivePy>(prim::kPrimHookBackward->name());
  fake_prim->set_hook(bprop_func);
  const auto &cell_id = GetCellId(cell, args);
  (void)fake_prim->AddAttr("cell_id", MakeValue(cell_id));
  (void)fake_prim->AddAttr(parse::CUSTOM_BPROP_NAME, MakeValue(true));

  py::object code_obj = py::getattr(bprop_func, "__code__");
  // Three parameters self, out and dout need to be excluded
  const size_t inputs_num = py::cast<int64_t>(py::getattr(code_obj, "co_argcount")) - 3;
  if (inputs_num > args.size()) {
    MS_EXCEPTION(TypeError) << "Size of bprop func inputs[" << inputs_num << "] is larger than size of cell inputs["
                            << args.size() << "]";
  }

  py::list cell_inputs;
  for (size_t i = 0; i < inputs_num; i += 1) {
    cell_inputs.append(args[i]);
  }
  OpExecInfoPtr op_exec_info = std::make_shared<OpExecInfo>();
  op_exec_info->op_name = fake_prim->name();
  op_exec_info->py_primitive = fake_prim;
  op_exec_info->op_inputs = cell_inputs;

  abstract::AbstractBasePtrList args_spec_list;
  std::vector<int64_t> op_masks;
  auto cnode = forward()->MakeCNode(op_exec_info, &op_masks, &args_spec_list);
  DoOpGrad(op_exec_info, cnode, out);
  const std::string out_obj_id = GetId(out);
  SaveOutputNodeMap(out_obj_id, out, cnode);
}

std::string GradExecutor::GetGradCellId(bool has_sens, const py::object &cell, const py::args &args,
                                        py::args *forward_args) {
  size_t forward_args_size = args.size();
  py::args tmp = args;
  if (has_sens) {
    forward_args_size--;
    py::tuple f_args(forward_args_size);
    for (size_t i = 0; i < forward_args_size; ++i) {
      f_args[i] = args[i];
    }
    tmp = f_args;
  }
  const auto &cell_id = GetCellId(cell, tmp);
  if (forward_args != nullptr) {
    *forward_args = tmp;
  }
  return cell_id;
}

void GradExecutor::GradNetInner(py::object *ret, const GradOperationPtr &grad, const py::object &cell,
                                const py::object &weights, const py::args &args) {
  MS_EXCEPTION_IF_NULL(grad);
  auto size = args.size();
  const auto &cell_id = GetGradCellId(grad->sens_param(), cell, args);
  MS_LOG(DEBUG) << "GradNet start " << size << " " << cell_id;
  if (!top_cell()->need_compile_graph()) {
    MS_LOG(DEBUG) << "No need compile graph";
    UpdateTopCellInfo(false, false, false);
    return;
  }

  auto df_builder = GetDfbuilder(cell_id);
  MS_EXCEPTION_IF_NULL(df_builder);
  auto resource = GetResource(cell_id);
  MS_EXCEPTION_IF_NULL(resource);
  MS_LOG(DEBUG) << "df_builder ptr " << df_builder.get() << " resource ptr " << resource.get();

  // Get params(weights) require derivative
  auto w_args = GetWeightsArgs(weights, df_builder);
  if (w_args.empty() && !df_builder->parameters().empty()) {
    MS_LOG(DEBUG) << "Add weights params to w_args";
    w_args.insert(w_args.end(), df_builder->parameters().begin(), df_builder->parameters().end());
  }
  // Get bprop graph of top cell
  auto bprop_graph = GetBpropGraph(grad, cell, w_args, size, args);
  resource->set_func_graph(bprop_graph);
  auto manager = resource->manager();
  MS_EXCEPTION_IF_NULL(manager);
  manager->AddFuncGraph(bprop_graph, true);
  DumpGraphIR("launch_bprop_graph.ir", bprop_graph);
  // Launch bprop graph to backend
  SaveForwardTensorInfoInBpropGraph(resource);
  resource->results()[pipeline::kBackend] = compile::CreateBackend();
  MS_LOG(DEBUG) << "Start task emit action";
  TaskEmitAction(resource);
  MS_LOG(DEBUG) << "Start execute action";
  ExecuteAction(resource);
  MS_LOG(DEBUG) << "Start update top cell info when run finish";
  UpdateTopCellInfo(false, false, true);
  resource->Clean();
}

std::vector<AnfNodePtr> GradExecutor::GetWeightsArgs(const py::object &weights, const FuncGraphPtr &df_builder) {
  MS_EXCEPTION_IF_NULL(df_builder);
  if (!py::hasattr(weights, "__parameter_tuple__")) {
    MS_LOG(DEBUG) << "No parameter tuple get";
    return {};
  }

  auto tuple = weights.cast<py::tuple>();
  MS_LOG(DEBUG) << "Get weights tuple size " << tuple.size();
  std::vector<AnfNodePtr> w_args;
  for (size_t it = 0; it < tuple.size(); ++it) {
    auto param = tuple[it];
    auto param_id = GetId(param);
    auto &graph_info_map = top_cell()->graph_info_map();
    if (graph_info_map.find(df_builder) == graph_info_map.end()) {
      MS_LOG(EXCEPTION) << "Can not find df_builder " << df_builder.get() << " Top cell " << top_cell().get()
                        << " cell id " << top_cell()->cell_id();
    }
    auto graph_info = graph_info_map.at(df_builder);
    MS_EXCEPTION_IF_NULL(graph_info);
    AnfNodePtr para_node = nullptr;
    if (graph_info->params.find(param_id) != graph_info->params.end()) {
      para_node = graph_info->params.at(param_id);
      w_args.emplace_back(para_node);
      continue;
    }
    auto name_attr = parse::python_adapter::GetPyObjAttr(param, "name");
    if (py::isinstance<py::none>(name_attr)) {
      MS_LOG(EXCEPTION) << "Parameter object should have name attribute";
    }
    auto param_name = py::cast<std::string>(name_attr);
    MS_LOG(DEBUG) << "The input " << it << " parameter weight name " << param_name;
    if (graph_info->params.find(param_name) != graph_info->params.end()) {
      para_node = graph_info->params.at(param_name);
    } else {
      MS_LOG(DEBUG) << "Can not find input param in graph info map, make a new parameter";
      auto free_param = df_builder->add_parameter();
      free_param->set_name(param_name);
      auto value = py::cast<tensor::TensorPtr>(param);
      free_param->set_default_param(value);
      free_param->debug_info()->set_name(param_name);
      para_node = free_param;
    }
    w_args.emplace_back(para_node);
  }
  return w_args;
}

abstract::AbstractBasePtrList GradExecutor::GetArgsSpec(const py::args &args, const FuncGraphPtr &bprop_graph) {
  MS_EXCEPTION_IF_NULL(bprop_graph);
  std::size_t size = args.size();
  abstract::AbstractBasePtrList args_spec;
  auto bprop_params = bprop_graph->parameters();
  if (bprop_params.size() < size) {
    MS_LOG(WARNING) << "Df parameters size " << bprop_params.size() << " less than " << size;
  }
  // Update abstract info for parameters in bprop graph
  size_t index = 0;
  for (const auto &param : bprop_params) {
    auto param_node = std::static_pointer_cast<Parameter>(param);
    MS_EXCEPTION_IF_NULL(param_node);
    if (param_node->has_default()) {
      // update abstract info for weights
      ValuePtr value = param_node->default_param();
      auto ptr = value->ToAbstract();
      MS_EXCEPTION_IF_NULL(ptr);
      args_spec.emplace_back(ptr);
      param_node->set_abstract(ptr->Broaden());
    } else {
      // update abstract info for input params
      ValuePtr input_value = parse::data_converter::PyDataToValue(args[index]);
      MS_EXCEPTION_IF_NULL(input_value);
      auto abs = abstract::FromValue(input_value, true);
      args_spec.emplace_back(abs);
      param_node->set_abstract(abs->Broaden());
      index++;
    }
  }
  MS_LOG(DEBUG) << "Args_spec size " << args_spec.size();
  return args_spec;
}

FuncGraphPtr GradExecutor::GetBpropGraph(const GradOperationPtr &grad, const py::object &cell,
                                         const std::vector<AnfNodePtr> &weights, size_t arg_size,
                                         const py::args &args) {
  bool build_formal_param_ = false;
  if ((!py::hasattr(cell, parse::CUSTOM_BPROP_NAME) && !cell_stack_.empty() && IsNestedGrad()) ||
      top_cell()->ms_function_flag()) {
    build_formal_param_ = true;
    need_renormalize_ = true;
  }

  auto k_pynative_cell_ptr = top_cell()->k_pynative_cell_ptr();
  MS_EXCEPTION_IF_NULL(k_pynative_cell_ptr);
  MS_EXCEPTION_IF_NULL(grad);
  FuncGraphPtr bprop_graph = ad::GradPynativeCellEnd(k_pynative_cell_ptr, weights, grad->get_all_, grad->get_by_list_,
                                                     grad->sens_param_, build_formal_param_);
  MS_EXCEPTION_IF_NULL(bprop_graph);

  MS_LOG(DEBUG) << "Top graph input params size " << arg_size;
  std::ostringstream ss;
  ss << "grad{" << arg_size << "}";
  bprop_graph->set_flag(FUNC_GRAPH_FLAG_CORE, true);
  bprop_graph->debug_info()->set_name(ss.str());
  // Get the parameters items and add the value to args_spec
  (void)GetArgsSpec(args, bprop_graph);

  // Do opt for final bprop graph
  ResourcePtr resource = std::make_shared<pipeline::Resource>();
  resource->set_func_graph(bprop_graph);
  auto manager = resource->manager();
  MS_EXCEPTION_IF_NULL(manager);
  manager->AddFuncGraph(bprop_graph);
  auto optimized_bg = pipeline::PrimBpropOptimizer::GetPrimBpropOptimizerInst().BpropGraphFinalOpt(resource);

  if (cell_stack_.empty()) {
    need_renormalize_ = false;
  }
  if (MsContext::GetInstance()->get_param<bool>(MS_CTX_SAVE_GRAPHS_FLAG)) {
    DumpIR("after_final_opt.ir", optimized_bg);
  }
  return optimized_bg;
}

py::object GradExecutor::CheckGraph(const py::object &cell, const py::args &args) {
  BaseRef ret = false;
  ++grad_order_;
  if (!grad_is_running_) {
    MS_LOG(DEBUG) << "Grad not running yet";
    return BaseRefToPyData(ret);
  }
  const auto &cell_id = GetCellId(cell, args);
  std::string key = cell_id.substr(0, std::min(PTR_LEN, cell_id.size()));
  MS_LOG(DEBUG) << "Key is " << key;
  if (top_cell_ != nullptr) {
    for (auto it = top_cell_->sub_cell_list().begin(); it != top_cell_->sub_cell_list().end(); ++it) {
      MS_LOG(DEBUG) << "Cur cell id " << *it;
      if ((*it).find(key) == std::string::npos) {
        continue;
      }
      MS_LOG(DEBUG) << "Delete cellid from cell graph list, top cell is " << top_cell_;
      top_cell_->sub_cell_list().erase(it);
      ret = true;
      break;
    }
  }
  return BaseRefToPyData(ret);
}

py::object PynativeExecutor::CheckAlreadyRun(const py::object &cell, const py::args &args) {
  bool forward_run = false;
  // Get cell id and input args info
  const auto &cell_id = grad_executor()->GetCellId(cell, args);
  std::string input_args_id;
  for (size_t i = 0; i < args.size(); ++i) {
    input_args_id = input_args_id + GetId(args[i]) + "_";
  }
  // Check whether need to run forward process
  auto top_cell = grad_executor()->GetTopCell(cell_id);
  if (top_cell != nullptr) {
    forward_run = top_cell->forward_already_run();
    grad_executor()->set_top_cell(top_cell);
    bool input_args_changed = !top_cell->input_args_id().empty() && top_cell->input_args_id() != input_args_id;
    if (forward_run && input_args_changed && top_cell->is_dynamic()) {
      MS_LOG(WARNING) << "The construct of running cell is dynamic and the input info of this cell has changed, "
                         "forward process will run again";
      forward_run = false;
    }
  }
  MS_LOG(DEBUG) << "Graph have already ran " << forward_run << " top cell id " << cell_id;
  return BaseRefToPyData(forward_run);
}

void GradExecutor::CheckNeedCompileGraph() {
  auto new_top_cell = top_cell();
  std::string top_cell_id = new_top_cell->cell_id();
  // update top cell by current cell op info
  if (already_run_top_cell_.find(top_cell_id) == already_run_top_cell_.end()) {
    MS_LOG(DEBUG) << "Top cell " << top_cell_id << " has never been ran, need compile graph";
    already_run_top_cell_[top_cell_id] = new_top_cell;
    return;
  }

  MS_LOG(DEBUG) << "Top cell " << top_cell_id << " has been ran";
  auto pre_top_cell = already_run_top_cell_.at(top_cell_id);
  auto pre_all_op_info = pre_top_cell->all_op_info();
  auto new_all_op_info = new_top_cell->all_op_info();
  MS_LOG(DEBUG) << "Pre all op info " << pre_all_op_info;
  MS_LOG(DEBUG) << "New all op info " << new_all_op_info;
  if (pre_all_op_info != new_all_op_info) {
    MS_LOG(DEBUG) << "The op info has been changed or new top cell has ms_function, need to compile graph again";
    EraseTopCellFromTopCellList(pre_top_cell);
    pre_top_cell->clear();
    already_run_top_cell_[top_cell_id] = new_top_cell;
  } else {
    MS_LOG(DEBUG) << "The op info has not been changed, no need to compile graph again";
    pre_top_cell->set_input_args_id(new_top_cell->input_args_id());
    EraseTopCellFromTopCellList(new_top_cell);
    new_top_cell->clear();
    pre_top_cell->set_forward_already_run(true);
    set_top_cell(pre_top_cell);
  }
}

void GradExecutor::RunGradGraph(py::object *ret, const py::object &cell, const py::tuple &args,
                                const py::object &phase) {
  MS_EXCEPTION_IF_NULL(ret);
  auto cell_id = GetCellId(cell, args);
  MS_LOG(DEBUG) << "Run start cell id " << cell_id;
  auto has_sens = std::any_of(top_cell_list_.begin(), top_cell_list_.end(), [&cell_id](const TopCellInfoPtr &value) {
    return cell_id.find(value->cell_id()) != std::string::npos && cell_id != value->cell_id();
  });
  py::args forward_args = args;
  cell_id = GetGradCellId(has_sens, cell, args, &forward_args);
  MS_LOG(DEBUG) << "Run has sens " << has_sens << " forward cell id " << cell_id;
  auto resource = GetResource(cell_id);
  MS_EXCEPTION_IF_NULL(resource);
  MS_LOG(DEBUG) << "Run resource ptr " << resource.get();

  VectorRef arg_list;
  py::tuple converted_args = ConvertArgs(args);
  pipeline::ProcessVmArgInner(converted_args, resource, &arg_list);
  if (resource->results().find(pipeline::kOutput) == resource->results().end()) {
    MS_LOG(EXCEPTION) << "Can't find run graph output";
  }
  if (!resource->results()[pipeline::kOutput].is<compile::VmEvalFuncPtr>()) {
    MS_LOG(EXCEPTION) << "Run graph is not VmEvalFuncPtr";
  }
  compile::VmEvalFuncPtr run = resource->results()[pipeline::kOutput].cast<compile::VmEvalFuncPtr>();
  MS_EXCEPTION_IF_NULL(run);

  std::string backend = MsContext::GetInstance()->backend_policy();
  MS_LOG(DEBUG) << "Eval run " << backend;
  grad_is_running_ = true;
  BaseRef value = (*run)(arg_list);
  grad_is_running_ = false;
  MS_LOG(DEBUG) << "Eval run end " << value.ToString();
  *ret = BaseRefToPyData(value);
  if (top_cell()->vm_compiled()) {
    MakeNestedCnode(cell, cell_id, forward_args, resource, *ret);
  } else if (GetHighOrderStackSize() >= 2) {
    SwitchTopcell();
  }
}

void GradExecutor::SwitchTopcell() {
  std::string inner_top_cell_all_op_info = top_cell()->all_op_info();
  bool inner_top_cell_is_dynamic = top_cell()->is_dynamic();
  top_cell()->set_grad_order(1);

  // Get outer top cell
  auto outer_top_cell = PopHighOrderGraphStack();
  MS_EXCEPTION_IF_NULL(outer_top_cell);
  std::string cur_all_op_info = outer_top_cell->all_op_info() + inner_top_cell_all_op_info;
  outer_top_cell->set_all_op_info(cur_all_op_info);
  // If inner is dynamic, outer set dynamic too
  if (inner_top_cell_is_dynamic) {
    outer_top_cell->set_is_dynamic(inner_top_cell_is_dynamic);
  }
  set_top_cell(outer_top_cell);
}

void GradExecutor::MakeNestedCnode(const py::object &cell, const std::string &cell_id, const py::args &forward_args,
                                   const ResourcePtr &resource, const py::object &out) {
  if (cell_stack_.empty()) {
    MS_LOG(DEBUG) << "No nested grad find";
    if (GetHighOrderStackSize() == 1) {
      MS_LOG(DEBUG) << "High order stack size is 1, pop high order stack";
      (void)PopHighOrderGraphStack();
    }
    return;
  }
  FuncGraphPtr first_grad_fg = nullptr;
  if (py::hasattr(cell, parse::CUSTOM_BPROP_NAME)) {
    first_grad_fg = curr_g_;
    MS_LOG(DEBUG) << "Bprop nested";
  } else {
    first_grad_fg = resource->func_graph();
  }
  MS_EXCEPTION_IF_NULL(first_grad_fg);
  DumpGraphIR("first_grad_fg.ir", first_grad_fg);

  auto out_id = GetId(out);
  ResourcePtr r = std::make_shared<pipeline::Resource>();
  r->manager()->AddFuncGraph(first_grad_fg);
  FuncGraphPtr second_grad_fg = ad::Grad(first_grad_fg, r);
  DumpGraphIR("second_grad_fg.ir", second_grad_fg);
  r->Clean();

  auto first_df_builder = GetDfbuilder(cell_id);
  MS_EXCEPTION_IF_NULL(first_df_builder);
  auto first_graph_info = top_cell()->graph_info_map().at(first_df_builder);
  MS_EXCEPTION_IF_NULL(first_graph_info);

  SwitchTopcell();

  auto second_df_builder = GetDfbuilder(top_cell()->cell_id());
  MS_EXCEPTION_IF_NULL(second_df_builder);
  for (const auto &it : first_graph_info->params) {
    SetParamNodeMapInGraphInfoMap(second_df_builder, it.first, it.second);
  }

  std::vector<AnfNodePtr> inputs{NewValueNode(first_grad_fg)};
  for (size_t i = 0; i < forward_args.size(); ++i) {
    inputs.emplace_back(GetInput(forward_args[i], false));
  }
  ValuePtrList weights_args;
  auto first_grad_all_params = first_grad_fg->parameters();
  for (const auto &it : first_grad_all_params) {
    auto p = it->cast<ParameterPtr>();
    MS_EXCEPTION_IF_NULL(p);
    if (!p->has_default()) {
      continue;
    }
    for (const auto &w : first_graph_info->params) {
      auto param = w.second;
      if (param->has_default() && param->name() == p->name()) {
        inputs.emplace_back(param);
        weights_args.emplace_back(param->default_param());
      }
    }
  }
  MS_LOG(DEBUG) << "Get pre graph ptr " << curr_g().get();
  auto cnode = curr_g_->NewCNode(inputs);
  SetTupleArgsToGraphInfoMap(curr_g_, out, cnode);
  SetNodeMapInGraphInfoMap(curr_g_, out_id, cnode);
  MS_LOG(DEBUG) << "Nested make cnode is " << cnode->DebugString(4);

  ValuePtrList input_args;
  for (size_t i = 0; i < forward_args.size(); ++i) {
    auto arg = parse::data_converter::PyDataToValue(forward_args[i]);
    MS_EXCEPTION_IF_NULL(arg);
    input_args.emplace_back(arg);
  }
  input_args.insert(input_args.end(), weights_args.begin(), weights_args.end());
  // Get output value
  py::object new_out;
  if (py::hasattr(cell, parse::CUSTOM_BPROP_NAME) && !py::isinstance<py::tuple>(out)) {
    new_out = py::make_tuple(out);
  } else {
    new_out = out;
  }
  auto out_value = parse::data_converter::PyDataToValue(new_out);
  MS_EXCEPTION_IF_NULL(out_value);
  // Add out and dout
  if (!top_cell()->k_pynative_cell_ptr()->KPynativeWithFProp(cnode, input_args, out_value, second_grad_fg)) {
    MS_LOG(EXCEPTION) << "Failed to run ad grad for second grad graph " << cnode->ToString();
  }
  need_renormalize_ = true;
}

void GradExecutor::EraseTopCellFromTopCellList(const TopCellInfoPtr &top_cell) {
  MS_EXCEPTION_IF_NULL(top_cell);
  auto iter = std::find_if(top_cell_list_.begin(), top_cell_list_.end(),
                           [&](const TopCellInfoPtr &elem) { return elem.get() == top_cell.get(); });
  if (iter == top_cell_list_.end()) {
    MS_LOG(EXCEPTION) << "Can not find top cell " << top_cell.get() << " cell id " << top_cell->cell_id()
                      << " from top cell list";
  }
  (void)top_cell_list_.erase(iter);
}

void GradExecutor::ClearGrad(const py::object &cell, const py::args &args) {
  if (grad_order_ > 0) {
    --grad_order_;
  }
  forward()->node_abs_map().clear();
  ad::CleanRes();
  pipeline::ReclaimOptimizer();
}

void GradExecutor::ClearRes() {
  MS_LOG(DEBUG) << "Clear grad res";
  grad_order_ = 0;
  grad_flag_ = false;
  need_renormalize_ = false;
  grad_is_running_ = false;
  top_cell_ = nullptr;
  curr_g_ = nullptr;
  bprop_cell_list_.clear();
  ClearCellRes();
  std::stack<bool>().swap(bprop_grad_stack_);
  std::stack<std::string>().swap(cell_stack_);
  std::stack<std::pair<FuncGraphPtr, TopCellInfoPtr>>().swap(high_order_stack_);
}

GradExecutorPtr PynativeExecutor::grad_executor() {
  MS_EXCEPTION_IF_NULL(grad_executor_);
  return grad_executor_;
}
ForwardExecutorPtr PynativeExecutor::forward_executor() {
  MS_EXCEPTION_IF_NULL(forward_executor_);
  return forward_executor_;
}

void PynativeExecutor::set_grad_flag(bool flag) { grad_executor()->set_grad_flag(flag); }

bool PynativeExecutor::GetIsDynamicCell() {
  if (grad_executor_ == nullptr) {
    return false;
  }
  return true;
}

py::object PynativeExecutor::CheckGraph(const py::object &cell, const py::args &args) {
  return grad_executor()->CheckGraph(cell, args);
}

py::object PynativeExecutor::Run(const py::object &cell, const py::tuple &args, const py::object &phase) {
  py::object ret;
  PynativeExecutorTry(grad_executor()->RunGraph, &ret, cell, args, phase);
  return ret;
}

void PynativeExecutor::ClearCell(const std::string &cell_id) {
  MS_LOG(DEBUG) << "Clear cell res, cell id " << cell_id;
  grad_executor()->ClearCellRes(cell_id);
}

void PynativeExecutor::ClearGrad(const py::object &cell, const py::args &args) {
  MS_LOG(DEBUG) << "Clear grad";
  return grad_executor()->ClearGrad(cell, args);
}

void PynativeExecutor::ClearRes() {
  MS_LOG(DEBUG) << "Clear all res";
  // Maybe exit in runop step
  auto ms_context = MsContext::GetInstance();
  if (ms_context != nullptr) {
    ms_context->set_param<bool>(MS_CTX_ENABLE_PYNATIVE_INFER, false);
  }
  ConfigManager::GetInstance().ResetIterNum();
  if (forward_executor_ != nullptr) {
    forward_executor_->ClearRes();
  }
  if (grad_executor_ != nullptr) {
    grad_executor_->ClearRes();
  }
  ad::CleanRes();
  pipeline::ReclaimOptimizer();
}

void PynativeExecutor::NewGraph(const py::object &cell, const py::args &args) {
  if (!grad_executor()->grad_flag()) {
    MS_LOG(DEBUG) << "Grad flag is false";
    return;
  }
  py::object *ret = nullptr;
  PynativeExecutorTry(grad_executor()->InitGraph, ret, cell, args);
}

void PynativeExecutor::EndGraph(const py::object &cell, const py::object &out, const py::args &args) {
  if (!grad_executor()->grad_flag()) {
    MS_LOG(DEBUG) << "Grad flag is false";
    return;
  }
  MS_LOG(DEBUG) << "Enter end graph process.";
  py::object *ret = nullptr;
  PynativeExecutorTry(grad_executor()->LinkGraph, ret, cell, out, args);
  MS_LOG(DEBUG) << "Leave end graph process.";
}

void PynativeExecutor::GradMsFunction(const py::object &out, const py::args &args) {
  if (!grad_executor()->grad_flag()) {
    MS_LOG(DEBUG) << "The grad flag is set to false, only run forward process. No need to make grad for ms_function";
    set_graph_phase("");
    return;
  }
  // Get ms_function graph by phase
  if (graph_phase().empty()) {
    MS_LOG(EXCEPTION) << "The graph phase is empty, can not obtain backend graph which is complied by ms_function";
  }
  MS_LOG(DEBUG) << "Ms_function graph phase: " << graph_phase();
  // Get ms_function graph
  auto executor = pipeline::ExecutorPy::GetInstance();
  MS_EXCEPTION_IF_NULL(executor);
  auto ms_func_graph = executor->GetFuncGraph(graph_phase());
  MS_EXCEPTION_IF_NULL(ms_func_graph);
  if (MsContext::GetInstance()->get_param<bool>(MS_CTX_SAVE_GRAPHS_FLAG)) {
    DumpIR("before_grad_ms_function.ir", ms_func_graph);
  }
  // Get fprop graph of ms_function
  ResourcePtr res = std::make_shared<pipeline::Resource>();
  res->set_func_graph(ms_func_graph);
  res->manager()->AddFuncGraph(ms_func_graph, true);
  auto fprop_g = ad::Grad(ms_func_graph, res, true);
  MS_EXCEPTION_IF_NULL(fprop_g);
  if (MsContext::GetInstance()->get_param<bool>(MS_CTX_SAVE_GRAPHS_FLAG)) {
    DumpIR("after_grad_ms_function.ir", fprop_g);
  }
  // Make adjoint for fprop graph of ms function graph
  grad_executor()->MakeAdjointForMsFunction(ms_func_graph, fprop_g, out, args, graph_phase());
  set_graph_phase("");
}

void PynativeExecutor::GradNet(const GradOperationPtr &grad, const py::object &cell, const py::object &weights,
                               const py::args &args) {
  py::object *ret = nullptr;
  PynativeExecutorTry(grad_executor()->GradGraph, ret, grad, cell, weights, args);
}

void PynativeExecutor::Sync() {
  if (session == nullptr) {
    MS_EXCEPTION(NotExistsError) << "No session has been created!";
  }
  session->SyncStream();
}

void PynativeExecutor::EnterConstruct(const py::object &cell) {
  if (py_top_cell_ != nullptr) {
    return;
  }
  py_top_cell_ = cell.ptr();
  MS_LOG(DEBUG) << "Enter construct process.";
}

void PynativeExecutor::LeaveConstruct(const py::object &cell) {
  if (py_top_cell_ != cell.ptr()) {
    return;
  }
  py_top_cell_ = nullptr;
  MS_LOG(DEBUG) << "Leave construct process.";
}

REGISTER_PYBIND_DEFINE(PynativeExecutor_, ([](const py::module *m) {
                         (void)py::class_<PynativeExecutor, std::shared_ptr<PynativeExecutor>>(*m, "PynativeExecutor_")
                           .def_static("get_instance", &PynativeExecutor::GetInstance, "PynativeExecutor get_instance.")
                           .def("new_graph", &PynativeExecutor::NewGraph, "pynative new a graph.")
                           .def("end_graph", &PynativeExecutor::EndGraph, "pynative end a graph.")
                           .def("check_graph", &PynativeExecutor::CheckGraph, "pynative check a grad graph.")
                           .def("check_run", &PynativeExecutor::CheckAlreadyRun, "pynative check graph run before.")
                           .def("grad_ms_function", &PynativeExecutor::GradMsFunction, "pynative grad for ms_function.")
                           .def("grad_net", &PynativeExecutor::GradNet, "pynative grad graph.")
                           .def("clear_cell", &PynativeExecutor::ClearCell, "pynative clear status.")
                           .def("clear_grad", &PynativeExecutor::ClearGrad, "pynative clear grad status.")
                           .def("sync", &PynativeExecutor::Sync, "pynative sync stream.")
                           .def("__call__", &PynativeExecutor::Run, "pynative executor run grad graph.")
                           .def("set_graph_phase", &PynativeExecutor::set_graph_phase, "pynative set graph phase")
                           .def("set_grad_flag", &PynativeExecutor::set_grad_flag, py::arg("flag") = py::bool_(false),
                                "Executor set grad flag.")
                           .def("enter_construct", &PynativeExecutor::EnterConstruct,
                                "Do something before enter construct function.")
                           .def("leave_construct", &PynativeExecutor::LeaveConstruct,
                                "Do something after leave construct function.");
                       }));
}  // namespace mindspore::pynative
