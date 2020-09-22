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

#include "pipeline/pynative/pynative_execute.h"

#include <typeinfo>
#include <map>
#include <set>
#include <memory>
#include <unordered_set>
#include <algorithm>

#include "debug/trace.h"
#include "pybind_api/ir/tensor_py.h"
#include "ir/param_info.h"
#include "ir/anf.h"
#include "ir/tensor.h"
#include "utils/any.h"
#include "utils/utils.h"
#include "utils/ms_context.h"
#include "utils/context/context_extends.h"
#include "utils/config_manager.h"
#include "utils/convert_utils_py.h"
#include "frontend/operator/ops.h"
#include "frontend/operator/composite/composite.h"
#include "frontend/operator/composite/do_signature.h"
#include "pipeline/jit/parse/data_converter.h"
#include "pipeline/jit/parse/parse_base.h"
#include "pipeline/jit/parse/resolve.h"
#include "pipeline/jit/static_analysis/prim.h"
#include "backend/session/session_factory.h"
#include "backend/optimizer/pass/const_input_to_attr_registry.h"
#include "backend/optimizer/common/helper.h"
#include "pipeline/jit/action.h"

#include "pipeline/pynative/base.h"
#include "pybind_api/api_register.h"
#include "vm/transform.h"

#include "frontend/optimizer/ad/grad.h"
#include "pipeline/jit/resource.h"
#include "pipeline/jit/pipeline.h"
#include "pipeline/jit/pass.h"

#ifdef ENABLE_GE
#include "pipeline/pynative/pynative_execute_ge.h"
#endif

#include "debug/anf_ir_dump.h"

using mindspore::tensor::TensorPy;

const char SINGLE_OP_GRAPH[] = "single_op_graph";
// primitive unable to infer value for constant input in PyNative mode
const std::set<std::string> vm_operators = {"make_ref", "HookBackward", "InsertGradientOf", "stop_gradient",
                                            "mixed_precision_cast"};

namespace mindspore {
namespace pynative {
static std::shared_ptr<session::SessionBasic> session = nullptr;
PynativeExecutorPtr PynativeExecutor::executor_ = nullptr;
std::mutex PynativeExecutor::instance_lock_;
ResourcePtr PynativeExecutor::resource_;
int PynativeExecutor::graph_id_ = 0;

template <typename... Args>
void PynativeExecutorTry(PynativeExecutor *const executor, void (PynativeExecutor::*method)(Args...), Args &&... args) {
  try {
    (executor->*method)(args...);
  } catch (const py::error_already_set &ex) {
    // print function call stack info before release
    std::ostringstream oss;
    trace::TraceGraphEval();
    trace::GetEvalStackInfo(oss);
    // call py::print to output function call stack to STDOUT, in case of output the log to file, the user can see
    // these info from screen, no need to open log file to find these info
    py::print(oss.str());
    MS_LOG(ERROR) << oss.str();
    PynativeExecutor::GetInstance()->Clean();
    // re-throw this exception to Python interpreter to handle it
    throw(py::error_already_set(ex));
  } catch (const py::type_error &ex) {
    PynativeExecutor::GetInstance()->Clean();
    throw py::type_error(ex);
  } catch (const py::value_error &ex) {
    PynativeExecutor::GetInstance()->Clean();
    throw py::value_error(ex);
  } catch (const py::index_error &ex) {
    PynativeExecutor::GetInstance()->Clean();
    throw py::index_error(ex);
  } catch (const std::exception &ex) {
    PynativeExecutor::GetInstance()->Clean();
    // re-throw this exception to Python interpreter to handle it
    throw(std::runtime_error(ex.what()));
  } catch (...) {
    PynativeExecutor::GetInstance()->Clean();
    std::string exName(abi::__cxa_current_exception_type()->name());
    MS_LOG(EXCEPTION) << "Error occurred when compile graph. Exception name: " << exName;
  }
}

inline ValuePtr PyAttrValue(const py::object &obj) {
  ValuePtr converted_ret = parse::data_converter::PyDataToValue(obj);
  if (!converted_ret) {
    MS_LOG(EXCEPTION) << "Attribute convert error with type:" << std::string(py::str(obj));
  }
  return converted_ret;
}

static std::string GetId(const py::object &obj) {
  py::object to_process = obj;
  std::string prefix = "";
  if (py::isinstance<py::tuple>(to_process) || py::isinstance<py::list>(to_process)) {
    auto p_list = py::cast<py::tuple>(to_process);
    if (p_list.empty()) {
      return "empty";
    }
    prefix = py::isinstance<py::tuple>(to_process) ? "tuple:" : "list";
    std::string key = "";
    for (size_t i = 0; i < p_list.size(); ++i) {
      key += std::string(py::str(GetId(p_list[i]))) + ":";
    }
    return prefix + key;
  }
  if (py::isinstance<mindspore::Type>(to_process)) {
    auto type_ptr = py::cast<mindspore::TypePtr>(to_process);
    return "type" + type_ptr->ToString();
  }
  if (py::isinstance<py::str>(to_process)) {
    return "s" + std::string(py::str(to_process));
  }
  if (py::isinstance<py::int_>(to_process)) {
    return prefix + std::string(py::str(to_process));
  }
  if (py::isinstance<py::float_>(to_process)) {
    return prefix + std::string(py::str(to_process));
  }
  if (py::isinstance<tensor::Tensor>(to_process)) {
    auto tensor_ptr = py::cast<tensor::TensorPtr>(to_process);
    return prefix + tensor_ptr->id();
  }

  py::object ret = parse::python_adapter::CallPyFn(parse::PYTHON_MOD_PARSE_MODULE, parse::PYTHON_MOD_GET_OBJ_ID, obj);
  return py::cast<std::string>(ret);
}

static std::string GetOpId(const OpExecInfoPtr &op_exec_info) {
  auto id = GetId(op_exec_info->py_primitive->GetPyObj());
  op_exec_info->prim_id = id;
  return id;
}

std::map<SignatureEnumDType, std::vector<size_t>> GetTypeIndex(const std::vector<SignatureEnumDType> &dtypes) {
  std::map<SignatureEnumDType, std::vector<size_t>> type_indexes;
  for (size_t i = 0; i < dtypes.size(); ++i) {
    auto it = type_indexes.find(dtypes[i]);
    if (it == type_indexes.end()) {
      (void)type_indexes.insert(std::make_pair(dtypes[i], std::vector<size_t>{i}));
    } else {
      it->second.push_back(i);
    }
  }
  return type_indexes;
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
    bool has_float = false;
    bool has_int = false;
    bool has_int8 = false;
    for (size_t index : indexes) {
      if (!has_float && py::isinstance<py::float_>(py_args[index])) {
        has_float = true;
      }
      if (!has_int && !py::isinstance<py::bool_>(py_args[index]) && py::isinstance<py::int_>(py_args[index])) {
        has_int = true;
      }

      auto obj = py_args[index];
      if (py::isinstance<tensor::Tensor>(obj)) {
        auto arg = py::cast<tensor::TensorPtr>(obj);
        TypeId arg_type_id = arg->data_type();
        auto type_priority = prim::type_map.find(arg_type_id);
        if (type_priority == prim::type_map.end()) {
          continue;
        }
        if (arg_type_id == kNumberTypeInt8) {
          has_int8 = true;
        }
        if (type_priority->second > priority) {
          max_type = type_priority->first;
          priority = type_priority->second;
        }
      }
    }
    if (max_type == TypeId::kNumberTypeBool) {
      if (has_int) {
        max_type = TypeId::kNumberTypeInt32;
      }
      if (has_float) {
        max_type = TypeId::kNumberTypeFloat32;
      }
    }
    if (max_type == TypeId::kNumberTypeUInt8 && has_int8) {
      max_type = TypeId::kNumberTypeInt16;
    }
    (void)dst_type.insert(std::make_pair(type, max_type));
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

py::object DoAutoCast(const py::object &arg, const TypeId &type_id) {
  py::tuple args(3);
  std::string module_name = "mindspore.ops.functional";
  std::string op_name = "cast";
  args[0] = parse::python_adapter::GetPyFn(module_name, op_name);
  args[1] = "Cast";

  std::string dst_type_str = TypeIdToMsTypeStr(type_id);
  module_name = "mindspore.common.dtype";
  py::object dst_type = parse::python_adapter::GetPyFn(module_name, dst_type_str);
  py::tuple inputs(2);
  inputs[0] = arg;
  inputs[1] = dst_type;
  args[2] = inputs;

  return RunOp(args)[0];
}

py::object DoParamMixPrecisionCast(bool *is_cast, const py::object obj) {
  auto tensor = py::cast<tensor::TensorPtr>(obj);
  auto cast_type = tensor->cast_dtype();
  py::object cast_output = obj;
  if (cast_type != nullptr) {
    auto source_element = tensor->Dtype();
    if (source_element != nullptr && IsSubType(source_element, kFloat) && *source_element != *cast_type) {
      MS_LOG(DEBUG) << "cast to " << cast_type->ToString();
      cast_output = DoAutoCast(obj, cast_type->type_id());
      *is_cast = true;
    }
  }
  return cast_output;
}

py::object DoParamMixPrecisionCastTuple(bool *is_cast, const py::tuple tuple) {
  auto tuple_size = static_cast<int>(tuple.size());
  py::tuple result(tuple_size);

  for (int i = 0; i < tuple_size; i++) {
    if (py::isinstance<tensor::MetaTensor>(tuple[i])) {
      MS_LOG(DEBUG) << "call cast for item " << i;
      result[i] = DoParamMixPrecisionCast(is_cast, tuple[i]);
    } else if (py::isinstance<py::tuple>(tuple[i])) {
      result[i] = DoParamMixPrecisionCastTuple(is_cast, tuple[i]);
    } else {
      result[i] = tuple[i];
    }
  }
  return result;
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

void DoSignatrueCast(const PrimitivePyPtr &prim, const std::map<SignatureEnumDType, TypeId> &dst_type,
                     const std::vector<SignatureEnumDType> &dtypes, const OpExecInfoPtr &op_exec_info) {
  const auto &signature = prim->signatures();
  auto &out_args = op_exec_info->op_inputs;
  bool has_dtype_sig = (dtypes.size() > 0);
  for (size_t i = 0; i < out_args.size(); ++i) {
    MS_LOG(DEBUG) << "check inputs " << i;
    auto obj = out_args[i];
    auto sig = SignatureEnumRW::kRWDefault;
    if (signature.size() > 0) {
      sig = signature[i].rw;
    }
    bool is_parameter = false;
    TypeId arg_type_id = kTypeUnknown;
    if (py::isinstance<tensor::MetaTensor>(obj)) {
      auto arg = py::cast<tensor::MetaTensorPtr>(obj);
      if (arg->is_parameter()) {
        is_parameter = true;
        MS_LOG(DEBUG) << "parameter is read " << i;
      }
      arg_type_id = arg->data_type();
    }

    // No need to implicit cast if no dtype.
    if (!has_dtype_sig || dtypes[i] == SignatureEnumDType::kDTypeEmptyDefaultValue) {
      continue;
    }
    auto it = dst_type.find(dtypes[i]);
    if (it == dst_type.end() || it->second == kTypeUnknown) {
      continue;
    }
    // implicit cast
    bool is_same_type = false;
    bool is_sig_write = (sig == SignatureEnumRW::kRWWrite);
    if (arg_type_id != 0) {
      is_same_type = (prim::type_map.find(arg_type_id) == prim::type_map.end() || arg_type_id == it->second);
    }
    if (is_sig_write) {
      if (!is_parameter) {
        prim::RaiseExceptionForCheckParameter(prim->name(), i, "not");
      }
      if (arg_type_id != 0) {
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
    py::object cast_output = DoAutoCast(out_args[i], it->second);
    out_args[i] = cast_output;
  }
}

void PynativeInfer(const PrimitivePyPtr &prim, const py::list &py_args, OpExecInfo *const op_exec_info,
                   const abstract::AbstractBasePtrList &args_spec_list) {
  MS_LOG(DEBUG) << "prim " << prim->name() << " input infer " << mindspore::ToString(args_spec_list);
  prim->BeginRecordAddAttr();
  AbstractBasePtr infer_res = EvalOnePrim(prim, args_spec_list)->abstract();
  prim->EndRecordAddAttr();
  op_exec_info->abstract = infer_res;
  MS_LOG(DEBUG) << "prim " << prim->name() << " infer result " << op_exec_info->abstract->ToString();
}

OpExecInfoPtr GenerateOpExecInfo(const py::args &args) {
  if (args.size() != PY_ARGS_NUM) {
    MS_LOG(ERROR) << "Three args are needed by RunOp";
    return nullptr;
  }
  auto op_exec_info = std::make_shared<OpExecInfo>();
  MS_EXCEPTION_IF_NULL(op_exec_info);
  op_exec_info->op_name = py::cast<std::string>(args[PY_NAME]);
  auto prim = py::cast<PrimitivePyPtr>(args[PY_PRIM]);
  if (!prim->HasPyObj()) {
    MS_LOG(EXCEPTION) << "pyobj is empty";
  }
  op_exec_info->py_primitive = prim;
  op_exec_info->op_attrs = py::getattr(args[PY_PRIM], "attrs");
  op_exec_info->op_inputs = args[PY_INPUTS];
  return op_exec_info;
}

std::string GetSingleOpGraphInfo(const OpExecInfoPtr &op_exec_info,
                                 const std::vector<tensor::TensorPtr> &input_tensors) {
  MS_EXCEPTION_IF_NULL(op_exec_info);
  std::string graph_info;
  // get input tensor info
  for (const auto &tensor : input_tensors) {
    MS_EXCEPTION_IF_NULL(tensor);
    auto tensor_shape = tensor->shape();
    (void)std::for_each(tensor_shape.begin(), tensor_shape.end(),
                        [&](const auto &dim) { (void)graph_info.append(std::to_string(dim) + "_"); });
    (void)graph_info.append(std::to_string(tensor->data_type()) + "_");
    if (tensor->device_address() != nullptr) {
      (void)graph_info.append(
        std::to_string(std::dynamic_pointer_cast<device::DeviceAddress>(tensor->device_address())->type_id()) + "_");
      (void)graph_info.append(std::dynamic_pointer_cast<device::DeviceAddress>(tensor->device_address())->format() +
                              "_");
    }
  }
  // get prim and abstract info
  (void)graph_info.append(op_exec_info->prim_id + "_");
  // get attr info
  const auto &op_prim = op_exec_info->py_primitive;
  MS_EXCEPTION_IF_NULL(op_prim);
  const auto &attr_map = op_prim->evaluate_added_attrs();
  (void)std::for_each(attr_map.begin(), attr_map.end(),
                      [&](const auto &element) { (void)graph_info.append(element.second->ToString() + "_"); });
  return graph_info;
}

py::object RunOpInVM(const OpExecInfoPtr &op_exec_info, PynativeStatusCode *status) {
  MS_LOG(INFO) << "RunOpInVM start";

  MS_EXCEPTION_IF_NULL(status);
  MS_EXCEPTION_IF_NULL(op_exec_info);
  MS_EXCEPTION_IF_NULL(op_exec_info->py_primitive);

  auto &op_inputs = op_exec_info->op_inputs;
  if (op_exec_info->op_name == "HookBackward" || op_exec_info->op_name == "InsertGradientOf") {
    py::tuple result(op_inputs.size());
    for (size_t i = 0; i < op_inputs.size(); i++) {
      py::object input = op_inputs[i];
      auto tensor = py::cast<tensor::TensorPtr>(input);
      auto new_tensor = std::make_shared<tensor::Tensor>(tensor->data_type(), tensor->shape(), tensor->data_ptr());
      new_tensor->set_device_address(tensor->device_address());
      new_tensor->set_sync_status(tensor->sync_status());
      result[i] = new_tensor;
    }
    *status = PYNATIVE_SUCCESS;
    MS_LOG(INFO) << "RunOpInVM end";
    return std::move(result);
  }
  auto primitive = op_exec_info->py_primitive;
  MS_EXCEPTION_IF_NULL(primitive);
  auto result = primitive->RunPyComputeFunction(op_inputs);
  if (py::isinstance<py::none>(result)) {
    MS_LOG(ERROR) << "VM got the result none, please check whether it is failed to get func";
    *status = PYNATIVE_OP_NOT_IMPLEMENTED_ERR;
    py::tuple err_ret(0);
    return std::move(err_ret);
  }

  // execute op
  py::tuple tuple_result = py::make_tuple(result);
  *status = PYNATIVE_SUCCESS;
  MS_LOG(INFO) << "RunOpInVM end";
  return std::move(tuple_result);
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
    input_tensors->push_back(tensor);
  }
  op_prim->set_attr(kAttrDynInputSizes, MakeValue(std::vector<int>{SizeToInt(tuple_inputs.size())}));
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
  input_tensors->push_back(tensor_ptr);
}

void ConvertMultiPyObjectToTensor(const py::object &input_object, const PrimitivePtr &op_prim,
                                  std::vector<tensor::TensorPtr> *input_tensors, int *tensor_mask) {
  MS_EXCEPTION_IF_NULL(op_prim);
  MS_EXCEPTION_IF_NULL(input_tensors);
  MS_EXCEPTION_IF_NULL(tensor_mask);

  if (!py::isinstance<py::tuple>(input_object)) {
    MS_LOG(EXCEPTION) << "The input should be a tuple!";
  }
  auto tuple_inputs = py::cast<py::tuple>(input_object);
  if (tuple_inputs.size() == 0) {
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
                             std::vector<tensor::TensorPtr> *input_tensors, int *tensor_mask) {
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
    tensor_ptr = std::make_shared<tensor::Tensor>(py::cast<py::int_>(input_object), kInt32);
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
  input_tensors->push_back(tensor_ptr);
}

void ConstructInputTensor(const OpExecInfoPtr &op_run_info, std::vector<int> *tensors_mask,
                          std::vector<tensor::TensorPtr> *input_tensors) {
  MS_EXCEPTION_IF_NULL(op_run_info);
  MS_EXCEPTION_IF_NULL(tensors_mask);
  MS_EXCEPTION_IF_NULL(input_tensors);
  PrimitivePtr op_prim = op_run_info->py_primitive;
  MS_EXCEPTION_IF_NULL(op_prim);

  opt::ConstInputToAttrInfoRegister reg;
  bool reg_exist = opt::ConstInputToAttrInfoRegistry::Instance().GetRegisterByOpName(op_run_info->op_name, &reg);
  if (op_run_info->op_name == prim::kPrimEmbeddingLookup->name()) {
    reg_exist = false;
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
    int tensor_mask = static_cast<int>(op_run_info->inputs_mask[index]);
    ConvertPyObjectToTensor(op_run_info->op_inputs[index], op_prim, input_tensors, &tensor_mask);
    // mark tensors, data : 0, weight : 1, valuenode: 2
    std::vector<int> new_mask(input_tensors->size() - tensors_mask->size(), tensor_mask);
    tensors_mask->insert(tensors_mask->end(), new_mask.begin(), new_mask.end());
  }
  op_prim->EndRecordAddAttr();
}

void EraseValueNodeTensor(const std::vector<int> &tensors_mask, std::vector<tensor::TensorPtr> *input_tensors) {
  MS_EXCEPTION_IF_NULL(input_tensors);
  if (input_tensors->size() != tensors_mask.size()) {
    MS_LOG(EXCEPTION) << "Input tensors size " << input_tensors->size() << " should be equal to tensors mask size "
                      << tensors_mask.size();
  }
  std::vector<tensor::TensorPtr> new_input_tensors;
  for (size_t index = 0; index < tensors_mask.size(); ++index) {
    if (tensors_mask[index] != kValueNodeTensorMask) {
      new_input_tensors.push_back(input_tensors->at(index));
    }
  }
  *input_tensors = new_input_tensors;
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

py::object RunOpInMs(const OpExecInfoPtr &op_exec_info, PynativeStatusCode *status) {
  MS_EXCEPTION_IF_NULL(op_exec_info);
  MS_LOG(INFO) << "Start run op[" << op_exec_info->op_name << "] with backend policy ms";
  auto ms_context = MsContext::GetInstance();
  ms_context->set_param<bool>(MS_CTX_ENABLE_PYNATIVE_INFER, true);
  std::string device_target = ms_context->get_param<std::string>(MS_CTX_DEVICE_TARGET);
  if (device_target != kAscendDevice && device_target != kGPUDevice) {
    MS_EXCEPTION(ArgumentError) << "Device target [" << device_target << "] is not supported in Pynative mode";
  }

  if (session == nullptr) {
    session = session::SessionFactory::Get().Create(device_target);
    MS_EXCEPTION_IF_NULL(session);
    session->Init(ms_context->get_param<uint32_t>(MS_CTX_DEVICE_ID));
  }

  std::vector<tensor::TensorPtr> input_tensors;
  std::vector<int> tensors_mask;
  ConstructInputTensor(op_exec_info, &tensors_mask, &input_tensors);
  // get graph info for checking it whether existing in the cache
  std::string graph_info = GetSingleOpGraphInfo(op_exec_info, input_tensors);
  session::OpRunInfo op_run_info = {op_exec_info->op_name, op_exec_info->py_primitive, op_exec_info->abstract,
                                    op_exec_info->value};
  session->BuildOpAsync(&op_run_info, graph_info, input_tensors, tensors_mask);
  EraseValueNodeTensor(tensors_mask, &input_tensors);
  VectorRef outputs;
  session->RunOpAsync(&op_run_info, graph_info, input_tensors, &outputs);
  auto result = BaseRefToPyData(outputs);
  ms_context->set_param<bool>(MS_CTX_ENABLE_PYNATIVE_INFER, false);
  *status = PYNATIVE_SUCCESS;
  MS_LOG(INFO) << "End run op[" << op_exec_info->op_name << "] with backend policy ms";
  return result;
}

py::object RunOpWithBackendPolicy(MsBackendPolicy backend_policy, const OpExecInfoPtr &op_exec_info,
                                  PynativeStatusCode *const status) {
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
      // use Ms fisrt,use others when ms failed
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

ValuePtr PynativeExecutor::GetForwardValue(const OpExecInfoPtr &op_exec_info) {
  auto id = GetOpId(op_exec_info);
  int graph_id = resource_->results()[pipeline::kPynativeGraphId].cast<int>();
  auto op = std::to_string(graph_id) + id;
  op.append(std::to_string(op_id_map_[id]));
  auto iter = op_forward_map_.find(op);
  if (iter != op_forward_map_.end()) {
    ++op_id_map_[id];
    MS_LOG(DEBUG) << "Get: " << op_exec_info->op_name << "(" << op << "), " << iter->second;
    return iter->second;
  }
  if (!first_grad_step_) {
    ++op_id_map_[id];
  }
  return nullptr;
}

AnfNodePtr PynativeExecutor::MakeCNode(const OpExecInfoPtr &op_exec_info, std::vector<bool> *op_masks,
                                       abstract::AbstractBasePtrList *args_spec_list) {
  MS_EXCEPTION_IF_NULL(op_masks);
  MS_EXCEPTION_IF_NULL(args_spec_list);
  CNodePtr cnode = nullptr;
  std::vector<AnfNodePtr> inputs;

  auto prim = op_exec_info->py_primitive;
  const auto &signature = prim->signatures();

  inputs.push_back(NewValueNode(prim));

  size_t size = op_exec_info->op_inputs.size();
  auto sig_size = signature.size();
  // ignore signature for cast op
  if (sig_size > 0 && sig_size != size) {
    MS_EXCEPTION(ValueError) << op_exec_info->op_name << " inputs size " << size << " does not match the requires "
                             << "inputs size " << sig_size;
  }
  bool is_cast_op = (op_exec_info->op_name == "Cast");
  if (!is_cast_op) {
    for (size_t i = 0; i < size; i++) {
      auto obj = op_exec_info->op_inputs[i];
      auto sig = SignatureEnumRW::kRWDefault;
      if (sig_size > 0) {
        sig = signature[i].rw;
      }
      MS_LOG(DEBUG) << "check mix precision " << op_exec_info->op_name << " input " << i << " "
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
        cast_output = DoParamMixPrecisionCast(&is_cast, obj);
      } else if (py::isinstance<py::tuple>(obj)) {
        // mix precision for tuple inputs
        cast_output = DoParamMixPrecisionCastTuple(&is_cast, obj);
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
    MS_LOG(DEBUG) << "do signature for " << op_exec_info->op_name;
    DoSignatrueCast(prim, dst_types, dtypes, op_exec_info);
  }
  MS_LOG(DEBUG) << "make cnode for " << op_exec_info->op_name;
  for (size_t i = 0; i < size; i++) {
    const auto &obj = op_exec_info->op_inputs[i];
    bool op_mask = false;
    if (py::isinstance<tensor::MetaTensor>(obj)) {
      auto meta_tensor = obj.cast<tensor::MetaTensorPtr>();
      if (meta_tensor) {
        op_mask = meta_tensor->is_parameter();
      }
    }
    (*op_masks).push_back(op_mask);
    MS_LOG(DEBUG) << "gen args i " << i << " " << op_exec_info->op_name << " op mask " << op_mask << " grad_flag_ "
                  << grad_flag_;

    AnfNodePtr node = nullptr;
    abstract::AbstractBasePtr abs = nullptr;
    auto id = GetId(obj);
    if (node_abs_map_.find(id) != node_abs_map_.end()) {
      abs = node_abs_map_[id];
    }
    if (!graph_info_map_.empty()) {
      node = GetInput(obj, op_mask);
    }
    if (node != nullptr && node->abstract() != nullptr) {
      abs = node->abstract();
    }

    auto const_input_index = prim->get_const_input_indexes();
    bool have_const_input = !const_input_index.empty();
    bool is_const_prim = prim->is_const_prim();
    MS_LOG(DEBUG) << prim->ToString() << " abs is nullptr " << (abs == nullptr) << " is_const_value "
                  << prim->is_const_prim();
    bool is_const_input = have_const_input && std::count(const_input_index.begin(), const_input_index.end(), i);
    if (abs == nullptr || is_const_prim || is_const_input) {
      MS_LOG(DEBUG) << "MakeCnode get node no in map" << id;
      ValuePtr input_value = PyAttrValue(obj);
      abs = input_value->ToAbstract();
      if (!is_const_prim && !is_const_input) {
        auto config = abstract::AbstractBase::kBroadenTensorOnly;
        abs = abs->Broaden(config);
        MS_LOG(DEBUG) << "broaden for " << prim->ToString() << " " << config;
      }
      node_abs_map_[id] = abs;
    }
    (*args_spec_list).push_back(abs);
    inputs.push_back(node);
  }

  MS_LOG(DEBUG) << "MakeCnode args end";
  if (grad_flag_) {
    if (curr_g_ != nullptr) {
      cnode = curr_g_->NewCNode(inputs);
      MS_LOG(DEBUG) << "MakeCnode set node " << cnode->DebugString(4);
    }
  }

  return cnode;
}

void PynativeExecutor::MakeCNode(const OpExecInfoPtr &op_exec_info, const py::object &out_real,
                                 const AnfNodePtr &cnode) {
  if (!grad_flag_ || graph_info_map_.empty()) {
    MS_LOG(DEBUG) << "no graph cnode";
    return;
  }

  std::string obj_id = GetId(out_real);
  MS_EXCEPTION_IF_NULL(cnode);
  MS_LOG(DEBUG) << "MakeCnode set obj node id " << cnode->DebugString(4) << "id " << obj_id;

  if (py::isinstance<py::tuple>(out_real)) {
    auto value = py::cast<py::tuple>(out_real);
    if (value.size() > 1) {
      for (int i = 0; i < static_cast<int>(value.size()); i++) {
        auto value_id = GetId(value[i]);
        MS_LOG(DEBUG) << "MakeCnode set node id " << value_id;
        set_obj_node_map(curr_g_, value_id, cnode, i);
      }
    }
  }
  set_obj_node_map(curr_g_, obj_id, cnode);
  set_pyobj(curr_g_, obj_id);
}

void GenTupleMap(const ValueTuplePtr &tuple, std::map<std::string, tensor::TensorPtr> *t_map) {
  if (t_map == nullptr) {
    return;
  }
  for (size_t i = 0; i < tuple->size(); i++) {
    ValuePtr tuple_i = (*tuple)[i];
    if (tuple_i->isa<tensor::Tensor>()) {
      auto t = tuple_i->cast<tensor::TensorPtr>();
      (*t_map)[t->id()] = t;
    } else if (tuple_i->isa<ValueTuple>()) {
      GenTupleMap(tuple_i->cast<ValueTuplePtr>(), t_map);
    }
  }
  MS_LOG(DEBUG) << "End GenTupleMap" << tuple->ToString();
}

ValuePtr CleanTupleAddr(const ValueTuplePtr &tuple) {
  std::vector<ValuePtr> value_list;
  for (size_t i = 0; i < tuple->size(); i++) {
    ValuePtr tuple_i = (*tuple)[i];
    if (tuple_i->isa<tensor::Tensor>()) {
      auto t = tuple_i->cast<tensor::TensorPtr>();
      auto new_tensor = std::make_shared<tensor::Tensor>(*t);
      new_tensor->set_device_address(nullptr);
      value_list.push_back(new_tensor);
    } else if (tuple_i->isa<ValueTuple>()) {
      value_list.push_back(CleanTupleAddr(tuple_i->cast<ValueTuplePtr>()));
    } else {
      MS_LOG(DEBUG) << "in value" << tuple_i->ToString();
      value_list.push_back(tuple_i);
    }
  }
  MS_LOG(DEBUG) << "End CleanTupleAddr";
  return std::make_shared<ValueTuple>(value_list);
}

void PynativeExecutor::SaveOpForwardValue(const std::string &id, const ValuePtr &value,
                                          std::map<std::string, tensor::TensorPtr> *t_map) {
  if (op_forward_map_.find(id) != op_forward_map_.end()) {
    if (op_forward_map_[id]->isa<ValueTuple>()) {
      // for one op have multi outputs but save only one tensor
      if (value->isa<tensor::Tensor>()) {
        auto tuple = op_forward_map_[id]->cast<ValueTuplePtr>();
        auto value_t = value->cast<tensor::TensorPtr>();
        for (size_t i = 0; i < tuple->size(); i++) {
          if ((*tuple)[i]->isa<tensor::Tensor>()) {
            auto tuple_t = (*tuple)[i]->cast<tensor::TensorPtr>();
            if (value_t->id() == tuple_t->id()) {
              tuple_t->set_device_address(value_t->device_address());
              MS_LOG(DEBUG) << "After Saveop " << tuple_t->ToString();
              break;
            }
          }
        }
      }
    }

    if (value->isa<ValueTuple>() && t_map != nullptr) {
      GenTupleMap(op_forward_map_[id]->cast<ValueTuplePtr>(), t_map);
    }
    MS_LOG(DEBUG) << "Save op forward value: "
                  << "(" << id << "), " << op_forward_map_[id]->ToString();
    return;
  }

  if (value->isa<ValueTuple>() && t_map == nullptr) {
    // make cnode gen all tuple node and set device_address be null
    op_forward_map_[id] = CleanTupleAddr(value->cast<ValueTuplePtr>());
  } else {
    op_forward_map_[id] = value;
  }
  MS_LOG(DEBUG) << "Save op forward value: "
                << "(" << id << "), " << value->ToString();
}

void PynativeExecutor::SaveAllResult(const OpExecInfoPtr &op_exec_info, const CNodePtr &cnode, const py::tuple &out) {
  if (!grad_flag_ || op_exec_info->value != nullptr) {
    return;
  }
  py::object out_real = out;
  if (out.size() == 1) {
    out_real = out[0];
  }
  auto value = PyAttrValue(out_real);
  if (cnode != nullptr) {
    size_t size = op_exec_info->op_inputs.size();
    for (size_t i = 0; i < size; i++) {
      auto obj = op_exec_info->op_inputs[i];
      auto obj_id = GetId(obj);
      if (obj_to_forward_id_.find(obj_id) != obj_to_forward_id_.end()) {
        cnode->add_input_value(PyAttrValue(obj), obj_to_forward_id_[obj_id]);
      } else {
        cnode->add_input_value(nullptr, "");
      }
    }
    std::string id = GetOpId(op_exec_info);
    int graph_id = resource_->results()[pipeline::kPynativeGraphId].cast<int>();
    auto op_id = std::to_string(graph_id) + id;
    op_id.append(std::to_string(op_id_map_[id]));
    cnode->set_forward(value, op_id);
    ++op_id_map_[id];
    auto out_id = GetId(out_real);
    if (py::isinstance<py::tuple>(out_real)) {
      auto tuple_item = py::cast<py::tuple>(out_real);
      for (size_t i = 0; i < tuple_item.size(); i++) {
        auto tuple_item_id = GetId(tuple_item[i]);
        obj_to_forward_id_[tuple_item_id] = op_id;
      }
      SaveOpForwardValue(op_id, value, nullptr);
    }
    obj_to_forward_id_[out_id] = op_id;
  }
}

AnfNodePtr PynativeExecutor::GetObjNode(const py::object &obj) {
  auto id = GetId(obj);
  auto &out = graph_info_map_[curr_g_].obj_node_map[id];
  if (out.second.size() == 1 && out.second[0] == -1) {
    return out.first;
  }
  CNodePtr node = out.first->cast<CNodePtr>();
  MS_LOG(DEBUG) << "output size " << out.second.size() << node->DebugString();
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
      MS_LOG(DEBUG) << "set tuple getitem abs" << prim_abs->ToString();
      node->set_abstract(prim_abs);
    }
  }
  if (node->abstract() != nullptr) {
    node_abs_map_[id] = node->abstract();
  }
  MS_LOG(DEBUG) << "GetObjNode output" << node->DebugString(6);
  return node;
}

AnfNodePtr PynativeExecutor::GetParamNode(const py::object &obj) {
  auto id = GetId(obj);
  auto &param = graph_info_map_[curr_g_].param_map[id];
  if (param.second.size() == 1 && param.second[0] == -1) {
    return param.first;
  }
  auto para_node = param.first;
  for (auto &idx : param.second) {
    std::vector<AnfNodePtr> tuple_get_item_inputs{NewValueNode(prim::kPrimTupleGetItem), para_node, NewValueNode(idx)};
    para_node = curr_g_->NewCNode(tuple_get_item_inputs);
  }
  return para_node;
}

std::string PynativeExecutor::GetCellId(const py::object &cell, const py::args &args) {
  auto cell_id = GetId(cell);
  for (size_t i = 0; i < args.size(); i++) {
    std::string arg_id = GetId(args[i]);
    if (node_abs_map_.find(arg_id) != node_abs_map_.end()) {
      cell_id += node_abs_map_[arg_id]->ToString();
    } else {
      auto abs = PyAttrValue(args[i])->ToAbstract();
      auto config = abstract::AbstractBase::kBroadenTensorOnly;
      abs = abs->Broaden(config);
      cell_id += abs->ToString();
      node_abs_map_[arg_id] = abs;
    }
  }
  return cell_id;
}

py::tuple PynativeExecutor::RunOpInner(const OpExecInfoPtr &op_exec_info) {
  MS_LOG(INFO) << "RunOp start, op name is: " << op_exec_info->op_name;
  mindspore::parse::python_adapter::set_python_env_flag(true);
  MsBackendPolicy backend_policy;
#if (!defined ENABLE_GE)
  auto ms_context = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(ms_context);
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
  PynativeStatusCode status = PYNATIVE_UNKNOWN_STATE;
  // returns a null py::tuple on error
  py::tuple err_ret(0);
  py::object result = RunOpWithBackendPolicy(backend_policy, op_exec_info, &status);
  if (status != PYNATIVE_SUCCESS) {
    MS_LOG(ERROR) << "Failed to run " << op_exec_info->op_name;
    return err_ret;
  }

  MS_LOG(DEBUG) << "RunOp end";
  return result;
}

py::tuple PynativeExecutor::RunOpInner(const py::args &args) {
  MS_LOG(DEBUG) << "RunOp start " << args.size();
  OpExecInfoPtr op_exec_info = nullptr;
  auto prim = py::cast<PrimitivePyPtr>(args[PY_PRIM]);
  auto name = py::cast<std::string>(args[PY_NAME]);
  abstract::AbstractBasePtrList args_spec_list;
  std::vector<bool> op_masks;
  op_exec_info = GenerateOpExecInfo(args);
  if (op_exec_info->op_name == prim::kPrimMixedPrecisionCast->name()) {
    return RunOpInner(op_exec_info);
  }
  auto cnode = PynativeExecutor::GetInstance()->MakeCNode(op_exec_info, &op_masks, &args_spec_list);
  bool is_find = false;
  if (prim_abs_list_.find(prim->id()) != prim_abs_list_.end()) {
    auto abs_list = prim_abs_list_[prim->id()];
    MS_LOG(DEBUG) << "match prim input args " << op_exec_info->op_name << mindspore::ToString(args_spec_list);
    if (abs_list.find(args_spec_list) != abs_list.end()) {
      MS_LOG(DEBUG) << "match prim ok" << op_exec_info->op_name;
      op_exec_info->abstract = abs_list[args_spec_list].abs;
      prim->set_evaluate_added_attrs(abs_list[args_spec_list].attrs);
      is_find = true;
    }
  }

  if (op_exec_info->abstract == nullptr) {
    // use python infer method
    if (ignore_infer_prim.find(op_exec_info->op_name) == ignore_infer_prim.end()) {
      PynativeInfer(prim, op_exec_info->op_inputs, op_exec_info.get(), args_spec_list);
    }
  }

  if (cnode != nullptr) {
    cnode->set_abstract(op_exec_info->abstract);
    MS_LOG(DEBUG) << "RunOp MakeCnode,new node is: " << cnode->DebugString();
  }

  op_exec_info->inputs_mask = op_masks;
  MS_EXCEPTION_IF_NULL(op_exec_info);
  if (op_exec_info->abstract != nullptr) {
    MS_LOG(DEBUG) << "run op infer" << name << op_exec_info->abstract->ToString();
    py::dict output = abstract::ConvertAbstractToPython(op_exec_info->abstract);
    if (!output["value"].is_none()) {
      py::tuple value_ret(1);
      value_ret[0] = output["value"];
      return value_ret;
    }
    if (op_exec_info->py_primitive->is_const_prim()) {
      py::tuple value_ret(1);
      value_ret[0] = "";
      return value_ret;
    }
  }

  if (!is_find) {
    // const_value need infer every step
    auto &out = prim_abs_list_[prim->id()];
    out[args_spec_list].abs = op_exec_info->abstract;
    out[args_spec_list].attrs = prim->evaluate_added_attrs();
    MS_LOG(DEBUG) << "set prim " << op_exec_info->op_name << mindspore::ToString(args_spec_list);
  }

  if (PynativeExecutor::GetInstance()->grad_flag()) {
    op_exec_info->value = PynativeExecutor::GetInstance()->GetForwardValue(op_exec_info);
  } else {
    (void)GetOpId(op_exec_info);
  }

  auto result = RunOpInner(op_exec_info);
  py::object out_real = result;
  if (result.size() == 1) {
    MS_LOG(DEBUG) << "MakeCnode out size is one.";
    out_real = result[0];
  }
  std::string obj_id = GetId(out_real);
  node_abs_map_[obj_id] = op_exec_info->abstract;
  PynativeExecutor::GetInstance()->MakeCNode(op_exec_info, out_real, cnode);
  if (cnode != nullptr) {
    PynativeExecutor::GetInstance()->SaveAllResult(op_exec_info, cnode->cast<CNodePtr>(), result);
  }
  return result;
}

py::tuple RunOp(const py::args &args) {
  try {
    return PynativeExecutor::GetInstance()->RunOpInner(args);
  } catch (const py::error_already_set &ex) {
    // print function call stack info before release
    std::ostringstream oss;
    trace::TraceGraphEval();
    trace::GetEvalStackInfo(oss);
    // call py::print to output function call stack to STDOUT, in case of output the log to file, the user can see
    // these info from screen, no need to open log file to find these info
    py::print(oss.str());
    MS_LOG(ERROR) << oss.str();
    PynativeExecutor::GetInstance()->Clean();
    // re-throw this exception to Python interpreter to handle it
    throw(py::error_already_set(ex));
  } catch (const py::type_error &ex) {
    PynativeExecutor::GetInstance()->Clean();
    throw py::type_error(ex);
  } catch (const py::value_error &ex) {
    PynativeExecutor::GetInstance()->Clean();
    throw py::value_error(ex);
  } catch (const py::index_error &ex) {
    PynativeExecutor::GetInstance()->Clean();
    throw py::index_error(ex);
  } catch (const std::exception &ex) {
    PynativeExecutor::GetInstance()->Clean();
    // re-throw this exception to Python interpreter to handle it
    throw(std::runtime_error(ex.what()));
  } catch (...) {
    PynativeExecutor::GetInstance()->Clean();
    std::string exName(abi::__cxa_current_exception_type()->name());
    MS_LOG(EXCEPTION) << "Error occurred when compile graph. Exception name: " << exName;
  }
}

void ClearPyNativeSession() { session = nullptr; }

PynativeExecutor::~PynativeExecutor() { ClearRes(); }

PynativeExecutor::PynativeExecutor() {
  grad_flag_ = false;
  first_grad_step_ = false;
}

void PynativeExecutor::NewGraphInner(const py::object &cell, const py::args &args) {
  auto cell_id = GetCellId(cell, args);
  // judge graph_context_.empty() to create sperate graphs except for the top
  if (cell_graph_map_.count(cell_id) != 0 && graph_context_.empty()) {
    if (cell_resource_map_.find(cell_id) != cell_resource_map_.end()) {
      resource_ = cell_resource_map_[cell_id];
    }
    MS_LOG(DEBUG) << "Newgraph already compiled";
    return;
  }

  auto g = std::make_shared<FuncGraph>();
  if (graph_context_.empty()) {
    for (auto arg : args) {
      if (py::isinstance<tensor::Tensor>(arg)) {
        auto tensor = arg.cast<tensor::TensorPtr>();
        if (tensor && tensor->is_parameter()) {
          MS_EXCEPTION(TypeError) << "The inputs could not be Parameter.";
        }
      }
    }
    // a df builder is built for every top function graph
    df_builder_ = std::make_shared<FuncGraph>();
    df_builder_map_[cell_id] = df_builder_;
    top_g_ = curr_g_ = g;
    resource_ = std::make_shared<pipeline::Resource>();
    resource_->results()[pipeline::kPynativeGraphId] = graph_id_++;
    cell_resource_map_[cell_id] = resource_;
    MS_LOG(DEBUG) << "First new graph" << top_g_.get();
    first_grad_step_ = true;
    top_graph_cells_.insert(cell_id);
  } else {
    if (df_builder_ == nullptr) {
      MS_LOG(EXCEPTION) << "In NewGraphInner, got df builder is nullptr";
    }
    curr_g_ = g;
  }
  Pushp();
  if (graph_info_map_.count(g) == 0) {
    graph_info_map_[g] = GraphInfo();
  }
  for (size_t i = 0; i < args.size(); i++) {
    auto param = args[i];
    auto new_param = g->add_parameter();
    std::string param_obj = GetId(param);
    if (py::isinstance<py::tuple>(param)) {
      auto tuple = param.cast<py::tuple>();
      auto tuple_size = static_cast<int>(tuple.size());
      for (int j = 0; j < tuple_size; j++) {
        set_param_map(curr_g_, GetId(tuple[j]), new_param, j);
        SetTupleParam(tuple[j], new_param, std::vector<int>{j});
      }
    }
    set_param_map(curr_g_, param_obj, new_param);
  }
}

AnfNodePtr PynativeExecutor::MakeValueNode(const py::object &obj, const std::string &obj_id) {
  ValuePtr converted_ret = nullptr;
  parse::ConvertData(obj, &converted_ret);
  auto node = NewValueNode(converted_ret);
  set_obj_node_map(curr_g_, obj_id, node);
  return node;
}

AnfNodePtr PynativeExecutor::GetInput(const py::object &obj, bool op_mask) {
  AnfNodePtr node = nullptr;
  std::string obj_id = GetId(obj);

  if (op_mask) {
    MS_LOG(DEBUG) << "Topgraph free parameter";
    // get the parameter name from parameter object
    auto name_attr = mindspore::parse::python_adapter::GetPyObjAttr(obj, "name");
    if (py::isinstance<py::none>(name_attr)) {
      MS_LOG(EXCEPTION) << "Parameter object should have name attribute";
    }
    auto param_name = py::cast<std::string>(name_attr);
    if (graph_info_map_[df_builder_].param_map.count(obj_id) == 0) {
      auto free_param = df_builder_->add_parameter();
      free_param->set_name(param_name);
      free_param->debug_info()->set_name(param_name);
      auto value = py::cast<tensor::TensorPtr>(obj);
      free_param->set_default_param(value);
      MS_LOG(DEBUG) << "Top graph set free parameter " << obj_id;
      set_param_map(df_builder_, obj_id, free_param);
      return free_param;
    }
    return graph_info_map_[df_builder_].param_map[obj_id].first;
  }
  // if input is graph output
  if (graph_info_map_[curr_g_].param_map.count(obj_id) != 0) {
    // op(x, y)
    node = GetParamNode(obj);
  } else if (graph_info_map_[curr_g_].obj_node_map.count(obj_id) != 0) {
    // out = op(op1(x, y))
    // out = op(cell1(x, y))
    // out = op(cell1(x, y)[0])
    node = GetObjNode(obj);
  } else if (py::isinstance<py::tuple>(obj)) {
    // out = op((x, y))
    // out = cell((x, y))
    auto tuple = obj.cast<py::tuple>();
    // cell((1,2)): support not mix (scalar, tensor)
    if (!tuple.empty() && !py::isinstance<tensor::Tensor>(tuple[0])) {
      return MakeValueNode(obj, obj_id);
    }
    std::vector<AnfNodePtr> args;
    args.push_back(NewValueNode(prim::kPrimMakeTuple));
    auto tuple_size = static_cast<int>(tuple.size());
    for (int i = 0; i < tuple_size; i++) {
      args.push_back(GetInput(tuple[i], false));
    }
    auto cnode = curr_g_->NewCNode(args);
    set_obj_node_map(curr_g_, GetId(obj), cnode);
    node = cnode;
  } else {
    node = MakeValueNode(obj, obj_id);
  }

  MS_LOG(DEBUG) << "Now getinput node " << node->ToString() << obj_id;
  return node;
}

// for output[0][1] need getitem multi
void PynativeExecutor::SetTupleOutput(const py::object &obj, const AnfNodePtr &cnode, std::vector<int> idx) {
  if (py::isinstance<py::tuple>(obj)) {
    auto tuple = obj.cast<py::tuple>();
    for (int i = 0; i < static_cast<int>(tuple.size()); i++) {
      std::vector<int> tmp = idx;
      tmp.push_back(i);
      set_obj_node_map(curr_g_, GetId(tuple[i]), cnode, tmp);
      SetTupleOutput(tuple[i], cnode, tmp);
    }
  }
}

// for param ((a, (b, c)), d) need multi getitem
void PynativeExecutor::SetTupleParam(const py::object &obj, const AnfNodePtr &para_node, std::vector<int> idx) {
  if (py::isinstance<py::tuple>(obj)) {
    auto tuple = obj.cast<py::tuple>();
    for (int i = 0; i < static_cast<int>(tuple.size()); i++) {
      std::vector<int> tmp = idx;
      tmp.push_back(i);
      set_param_map(curr_g_, GetId(tuple[i]), para_node, tmp);
      SetTupleParam(tuple[i], para_node, tmp);
    }
  }
}

void PynativeExecutor::Pushp() { graph_context_.push(curr_g_); }

void PynativeExecutor::Popp() {
  if (graph_context_.empty()) {
    MS_LOG(EXCEPTION) << "Stack graph_context_ is empty";
  }
  graph_context_.pop();
  if (!graph_context_.empty()) {
    curr_g_ = graph_context_.top();
  }
}

void PynativeExecutor::EndGraphInner(const py::object &cell, const py::object &out, const py::args &args) {
  auto cell_id = GetCellId(cell, args);
  if (cell_graph_map_.count(cell_id) != 0 && graph_context_.empty()) {
    MS_LOG(DEBUG) << "Endgraph already compiled";
    return;
  }

  cell_graph_map_[cell_id] = curr_g_;
  auto out_id = GetId(out);
  if (!graph_info_map_[curr_g_].obj_node_map.count(out_id) && !graph_info_map_[curr_g_].param_map.count(out_id)) {
    // cell construct return x, y
    if (py::isinstance<py::tuple>(out)) {
      std::vector<AnfNodePtr> args;
      args.push_back(NewValueNode(prim::kPrimMakeTuple));

      auto tuple = out.cast<py::tuple>();
      MS_LOG(DEBUG) << "End graph start tuple size" << tuple.size();
      auto tuple_size = static_cast<int>(tuple.size());
      auto cnode = curr_g_->NewCNode(args);
      for (int i = 0; i < tuple_size; i++) {
        args.push_back(GetInput(tuple[i], false));
      }
      cnode->set_inputs(args);

      for (int i = 0; i < tuple_size; i++) {
        set_obj_node_map(curr_g_, GetId(tuple[i]), cnode, i);
        SetTupleOutput(tuple[i], cnode, std::vector<int>{i});
      }
      set_obj_node_map(curr_g_, out_id, cnode);
    } else {
      MS_LOG(DEBUG) << "Set ValueNode as output for graph, out id: " << out_id;
      MakeValueNode(out, out_id);
    }
  }
  EndGraphByOutId(out_id, cell, out, args);
}

void PynativeExecutor::EndGraphByOutId(const std::string &out_id, const py::object &cell, const py::object &out,
                                       const py::args &args) {
  AnfNodePtr output_node;
  if (graph_info_map_[curr_g_].param_map.count(out_id)) {
    output_node = GetParamNode(out);
  } else {
    output_node = GetObjNode(out);
  }
  curr_g_->set_output(output_node);
  std::vector<AnfNodePtr> inputs;
  inputs.push_back(NewValueNode(curr_g_));
  MS_LOG(DEBUG) << "Current graph" << curr_g_->output()->DebugString();
  resource_->manager()->AddFuncGraph(curr_g_);
  // custom bprop debug
  bool need_replace_param = false;
  if (py::hasattr(cell, parse::CUSTOM_BPROP_NAME)) {
    need_replace_param = true;
    size_t par_number = py::tuple(parse::python_adapter::CallPyObjMethod(cell, "get_parameters")).size();
    if (par_number > 0) {
      MS_LOG(EXCEPTION) << "When user defines the net bprop, there are " << par_number
                        << " parameters that is not supported in the net.";
    }
    MS_LOG(DEBUG) << "Use cell custom bprop function.";
    FuncGraphPtr bprop_graph = parse::ConvertToBpropCut(cell);
    if (bprop_graph != nullptr) {
      (void)curr_g_->transforms().insert(std::make_pair(parse::CUSTOM_BPROP_NAME, FuncGraphTransform(bprop_graph)));
      (void)bprop_graph->transforms().insert(std::make_pair("primal", FuncGraphTransform(curr_g_)));
    }
  }
  auto newfg = ad::Grad(curr_g_, resource_, graph_context_.size() == 1);
  if (need_replace_param) {
    auto params = newfg->parameters();
    auto manager = Manage({newfg}, false);
    if (args.size() > params.size()) {
      MS_EXCEPTION(TypeError) << "The number of arguments " << args.size()
                              << " is more than the number of parameters required, which is " << params.size();
    }
    for (size_t i = 0; i < args.size(); i++) {
      ValuePtr value = PyAttrValue(args[i]);
      auto v_node = NewValueNode(value);
      manager->Replace(params[i], v_node);
    }
  }
  graph_info_map_.erase(curr_g_);
  if (graph_context_.size() > 1) {
    Popp();
    // connect the previous graph to the inside graph
    auto graph_prev = graph_context_.top();
    for (size_t i = 0; i < args.size(); i++) {
      auto input = GetInput(args[i], false);
      inputs.push_back(input);
    }
    auto out_cnode = graph_prev->NewCNode(inputs);
    set_pyobj(graph_prev, GetCellId(cell, args));
    if (py::isinstance<py::tuple>(out)) {
      auto out_list = py::cast<py::tuple>(out);
      auto out_size = static_cast<int>(out_list.size());
      for (int i = 0; i < out_size; i++) {
        set_obj_node_map(graph_prev, GetId(out_list[i]), out_cnode, i);
        SetTupleOutput(out_list[i], out_cnode, std::vector<int>{i});
      }
    }
    set_obj_node_map(graph_prev, GetId(out), out_cnode);
  } else {
    if (MsContext::GetInstance()->get_param<bool>(MS_CTX_SAVE_GRAPHS_FLAG)) {
      DumpIR("before_resolve.ir", newfg);
    }
    parse::ResolveFuncGraph(newfg, resource_);
    if (MsContext::GetInstance()->get_param<bool>(MS_CTX_SAVE_GRAPHS_FLAG)) {
      DumpIR("after_resolve.ir", newfg);
    }
    resource_->set_func_graph(newfg);
    Popp();
  }
}

std::vector<AnfNodePtr> PynativeExecutor::GetWeightsArgs(const py::object &weights) {
  std::vector<AnfNodePtr> w_args;
  if (py::hasattr(weights, "__parameter_tuple__")) {
    auto tuple = weights.cast<py::tuple>();
    MS_LOG(DEBUG) << "GradNet start weights tuple size" << tuple.size();
    w_args.push_back(NewValueNode(prim::kPrimMakeTuple));
    for (size_t it = 0; it < tuple.size(); ++it) {
      auto param = tuple[it];
      auto param_id = GetId(param);
      AnfNodePtr para_node = nullptr;
      if (graph_info_map_[df_builder_].param_map.count(param_id)) {
        para_node = graph_info_map_[df_builder_].param_map[param_id].first;
      } else {
        auto name_attr = mindspore::parse::python_adapter::GetPyObjAttr(param, "name");
        if (py::isinstance<py::none>(name_attr)) {
          MS_LOG(EXCEPTION) << "Parameter object should have name attribute";
        }
        auto param_name = py::cast<std::string>(name_attr);
        auto free_param = df_builder_->add_parameter();
        free_param->set_name(param_name);
        auto value = py::cast<tensor::TensorPtr>(param);
        free_param->set_default_param(value);
        free_param->debug_info()->set_name(param_name);
        para_node = free_param;
      }
      w_args.push_back(para_node);
    }
  } else {
    MS_LOG(DEBUG) << "training not paramter_tuple";
  }
  return w_args;
}

abstract::AbstractBasePtrList PynativeExecutor::GetArgsSpec(const py::args &args) {
  abstract::AbstractBasePtrList args_spec;
  std::size_t size = args.size();
  for (std::size_t i = 0; i < size; i++) {
    ValuePtr converted = nullptr;
    bool succ = parse::ConvertData(args[i], &converted);
    if (!succ) {
      MS_LOG(EXCEPTION) << "Args convert error";
    }
    bool broaden = true;
    auto abs = abstract::FromValue(converted, broaden);
    args_spec.push_back(abs);
    auto param_node = std::static_pointer_cast<Parameter>(df_builder_->parameters()[i]);
    param_node->set_abstract(abs);
  }

  for (const auto &param : df_builder_->parameters()) {
    auto param_node = std::static_pointer_cast<Parameter>(param);
    if (param_node->has_default()) {
      ValuePtr value = param_node->default_param();
      auto ptr = value->ToAbstract();
      if (ptr == nullptr) {
        MS_LOG(EXCEPTION) << "Args convert error";
      }
      args_spec.push_back(ptr);
      param_node->set_abstract(ptr);
    }
  }

  return args_spec;
}

void PynativeExecutor::GradNetInner(const GradOperationPtr &grad, const py::object &cell, const py::object &weights,
                                    const py::args &args) {
  MS_LOG(INFO) << "GradNet start" << args.size();
  std::size_t size = args.size();
  std::string cell_id = GetCellId(cell, args);
  if (graph_map_.count(cell_id) != 0) {
    MS_LOG(DEBUG) << "GradNet already compiled";
    return;
  }
  size_t forward_args_count = args.size();
  if (grad->sens_param()) {
    forward_args_count = forward_args_count - 1;
  }
  py::tuple forward_args(forward_args_count);
  for (size_t i = 0; i < forward_args_count; i++) {
    forward_args[i] = args[i];
  }
  std::string forward_cell_id = GetCellId(cell, forward_args);
  MS_LOG(DEBUG) << "Forward cell_id:" << forward_cell_id;
  if (df_builder_map_.find(forward_cell_id) == df_builder_map_.end()) {
    MS_LOG(EXCEPTION) << "Cannot find df builder";
  }
  df_builder_ = df_builder_map_[forward_cell_id];
  if (df_builder_ == nullptr) {
    MS_LOG(EXCEPTION) << "Got unexpected null df builder";
  }

  if (cell_resource_map_.find(forward_cell_id) == cell_resource_map_.end()) {
    MS_LOG(EXCEPTION) << "Cannot find resource for " << forward_cell_id;
  }
  MS_LOG(DEBUG) << "GradNet first compiled";
  resource_ = cell_resource_map_[forward_cell_id];

  std::vector<AnfNodePtr> new_params;
  for (size_t i = 0; i < size; i++) {
    ParameterPtr p = std::make_shared<Parameter>(df_builder_);
    new_params.push_back(p);
  }
  MS_LOG(DEBUG) << "GradNet start weight size" << df_builder_->parameters().size();
  new_params.insert(new_params.end(), df_builder_->parameters().begin(), df_builder_->parameters().end());
  df_builder_->set_parameters(new_params);
  resource_->manager()->SetParameters(df_builder_, new_params);

  std::vector<AnfNodePtr> w_args = GetWeightsArgs(weights);
  MS_EXCEPTION_IF_NULL(resource_->func_graph());
  if (cell_graph_map_.find(forward_cell_id) == cell_graph_map_.end()) {
    MS_LOG(EXCEPTION) << "Could not find top graph by cellid: " << forward_cell_id;
  }
  top_g_ = cell_graph_map_[forward_cell_id];
  if (MsContext::GetInstance()->get_param<bool>(MS_CTX_SAVE_GRAPHS_FLAG)) {
    DumpIR("before_grad.ir", resource_->func_graph());
  }
  auto g = GradGraph(resource_->func_graph(), grad, w_args, size);
  if (MsContext::GetInstance()->get_param<bool>(MS_CTX_SAVE_GRAPHS_FLAG)) {
    DumpIR("after_grad.ir", g);
  }
  resource_->set_func_graph(g);
  resource_->manager()->KeepRoots({g});

  // get the parameters items and add the value to args_spec
  abstract::AbstractBasePtrList args_spec = GetArgsSpec(args);
  MS_LOG(DEBUG) << "Args_spec size" << args_spec.size();

  resource_->set_args_spec(args_spec);
  MS_LOG(DEBUG) << "Start opt";

  // Create backend and session
  resource_->results()[pipeline::kBackend] = compile::CreateBackend();

  graph_map_[cell_id] = g;
  PynativeOptimizeAction(resource_);
  TaskEmitAction(resource_);
  ExecuteAction(resource_);
  resource_->Clean();
  ad::CleanRes();
  pipeline::ReclaimOptimizer();
}

template <typename T>
void MapClear(T *map, const std::string &flag) {
  for (auto it = map->begin(); it != map->end();) {
    if (it->first.find(flag) != std::string::npos) {
      it->second = nullptr;
      it = map->erase(it);
    } else {
      it++;
    }
  }
}

void PynativeExecutor::Clear(const std::string &flag) {
  if (!flag.empty()) {
    MS_LOG(DEBUG) << "Clear res";
    MapClear<std::unordered_map<std::string, FuncGraphPtr>>(&graph_map_, flag);
    MapClear<std::unordered_map<std::string, FuncGraphPtr>>(&cell_graph_map_, flag);
    MapClear<std::unordered_map<std::string, ResourcePtr>>(&cell_resource_map_, flag);
    MapClear<std::unordered_map<std::string, FuncGraphPtr>>(&df_builder_map_, flag);

    // Maybe exit in the pynative runing op, so need reset pynative flag.
    auto ms_context = MsContext::GetInstance();
    if (ms_context != nullptr) {
      ms_context->set_param<bool>(MS_CTX_ENABLE_PYNATIVE_INFER, false);
    }
    ConfigManager::GetInstance().ResetIterNum();
    if (top_graph_cells_.find(flag) != top_graph_cells_.end()) {
      op_forward_map_.clear();
      Clean();
    }
    return;
  }

  MS_LOG(DEBUG) << "Clear";
  grad_flag_ = false;
  top_g_ = nullptr;
  df_builder_ = nullptr;
  curr_g_ = nullptr;
  first_grad_step_ = false;
  graph_info_map_.clear();
  op_id_map_.clear();
  obj_to_forward_id_.clear();
  std::stack<FuncGraphPtr>().swap(graph_context_);
  ConfigManager::GetInstance().ResetIterNum();
}

void PynativeExecutor::Clean() {
  MS_LOG(DEBUG) << "Clean all res";
  Clear();
  grad_flag_ = false;
  ad::CleanRes();
  pipeline::ReclaimOptimizer();
}

template <typename T>
void MapErase(T *map) {
  for (auto it = map->begin(); it != map->end();) {
    it = map->erase(it++);
  }
}

void PynativeExecutor::ClearRes() {
  MapErase<std::unordered_map<std::string, FuncGraphPtr>>(&graph_map_);
  MapErase<std::unordered_map<std::string, FuncGraphPtr>>(&cell_graph_map_);
  MapErase<std::unordered_map<std::string, ResourcePtr>>(&cell_resource_map_);
  MapErase<std::unordered_map<std::string, abstract::AbstractBasePtr>>(&node_abs_map_);
  Clean();
  resource_.reset();
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

py::object PynativeExecutor::Run(const py::tuple &args, const py::object &phase) {
  VectorRef arg_list;
  py::tuple converted_args = ConvertArgs(args);
  pipeline::ProcessVmArgInner(converted_args, resource_, &arg_list);
  if (resource_->results().find(pipeline::kOutput) == resource_->results().end() ||
      !resource_->results()[pipeline::kOutput].is<compile::VmEvalFuncPtr>()) {
    MS_LOG(EXCEPTION) << "Can't find run graph func for ";
  }
  compile::VmEvalFuncPtr run = resource_->results()[pipeline::kOutput].cast<compile::VmEvalFuncPtr>();
  if (run == nullptr) {
    MS_LOG(EXCEPTION) << "Can't find run graph func for ";
  }

  std::string backend = MsContext::GetInstance()->backend_policy();
  MS_LOG(DEBUG) << "Eval run" << backend;
  BaseRef value = (*run)(arg_list);
  MS_LOG(DEBUG) << "Run end" << value.ToString();
  return BaseRefToPyData(value);
}

FuncGraphPtr PynativeExecutor::GradGraph(FuncGraphPtr g, const GradOperationPtr &grad_op,
                                         const std::vector<AnfNodePtr> &weights, size_t arg_size) {
  auto nparam = top_g_->parameters().size();
  std::ostringstream ss;
  ss << "grad{" << nparam << "}";
  df_builder_->set_flag(FUNC_GRAPH_FLAG_CORE, true);
  df_builder_->debug_info()->set_name(ss.str());

  auto df = grad_op->GetGrad(NewValueNode(g), nullptr, top_g_->parameters(), weights);
  std::vector<AnfNodePtr> inputs = {NewValueNode(df)};
  for (size_t i = 0; i < arg_size; ++i) {
    inputs.push_back(df_builder_->parameters()[i]);
  }
  auto out = df_builder_->NewCNode(inputs);
  df_builder_->set_output(out);
  resource_->manager()->AddFuncGraph(df);
  resource_->manager()->AddFuncGraph(df_builder_);
  return df_builder_;
}

void PynativeExecutor::NewGraph(const py::object &cell, const py::args &args) {
  PynativeExecutorTry(this, &PynativeExecutor::NewGraphInner, cell, args);
}

void PynativeExecutor::EndGraph(const py::object &cell, const py::object &out, const py::args &args) {
  PynativeExecutorTry(this, &PynativeExecutor::EndGraphInner, cell, out, args);
}

void PynativeExecutor::GradNet(const GradOperationPtr &grad, const py::object &cell, const py::object &weights,
                               const py::args &args) {
  PynativeExecutorTry(this, &PynativeExecutor::GradNetInner, grad, cell, weights, args);
}

REGISTER_PYBIND_DEFINE(PynativeExecutor_, ([](const py::module *m) {
                         (void)py::class_<PynativeExecutor, std::shared_ptr<PynativeExecutor>>(*m, "PynativeExecutor_")
                           .def_static("get_instance", &PynativeExecutor::GetInstance, "PynativeExecutor get_instance.")
                           .def("new_graph", &PynativeExecutor::NewGraph, "pynative new a graph.")
                           .def("end_graph", &PynativeExecutor::EndGraph, "pynative end a graph.")
                           .def("grad_net", &PynativeExecutor::GradNet, "pynative grad graph.")
                           .def("clear", &PynativeExecutor::Clear, "pynative clear status.")
                           .def("__call__", &PynativeExecutor::Run, py::arg("args"), py::arg("phase") = py::str(""),
                                "Executor run function.")
                           .def("set_grad_flag", &PynativeExecutor::set_grad_flag, py::arg("flag") = py::bool_(false),
                                "Executor set grad flag.");
                       }));
}  // namespace pynative
}  // namespace mindspore
