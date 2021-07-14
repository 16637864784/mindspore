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

#ifndef MINDSPORE_CCSRC_PIPELINE_PYNATIVE_PYNATIVE_EXECUTE_H_
#define MINDSPORE_CCSRC_PIPELINE_PYNATIVE_PYNATIVE_EXECUTE_H_

#include <utility>
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <stack>
#include <set>
#include <map>

#include "pybind11/pybind11.h"
#include "pybind11/numpy.h"

#include "pybind_api/ir/base_ref_py.h"
#include "pipeline/pynative/base.h"
#include "utils/ms_context.h"
#include "ir/anf.h"
#include "pipeline/jit/resource.h"
#include "frontend/optimizer/ad/kpynative.h"
#include "frontend/operator/composite/composite.h"
#include "pipeline/pynative/pynative_abs_cache.h"

namespace mindspore::pynative {
namespace py = pybind11;
using CellId = std::string;
using MsFunctionGradCache = std::unordered_map<std::string, std::pair<FuncGraphPtr, FuncGraphPtr>>;
using OpInfoWithTensorId = std::unordered_map<std::string, std::vector<std::string>>;
using TensorIdWithTensorObject = std::unordered_map<std::string, std::vector<tensor::TensorPtr>>;

py::object RealRunOp(const py::args &args);

struct GraphInfo {
  std::string cell_id;
  AnfNodePtr output;
  OrderedMap<std::string, ParameterPtr> params;  // hold input parameters and cell weights
  std::unordered_map<std::string, std::pair<AnfNodePtr, std::vector<int64_t>>> node_map;
  GraphInfo() = default;
  explicit GraphInfo(std::string id) : cell_id(std::move((id))) {}
};
using GraphInfoPtr = std::shared_ptr<GraphInfo>;

class TopCellInfo {
 public:
  TopCellInfo() = default;
  ~TopCellInfo() = default;
  TopCellInfo(bool topest, size_t grad_order, pipeline::ResourcePtr r, FuncGraphPtr df, std::string cellid)
      : is_topest_(topest),
        grad_order_(grad_order),
        resource_(std::move(r)),
        df_builder_(std::move(df)),
        cell_id_(std::move(cellid)) {}

  bool is_init_kpynative() const { return is_init_kpynative_; }
  void set_init_kpynative(bool init) { is_init_kpynative_ = init; }
  bool is_topest() const { return is_topest_; }
  size_t grad_order() const { return grad_order_; }
  void set_grad_order(size_t grad_order) { grad_order_ = grad_order; }
  bool is_dynamic() const { return is_dynamic_; }
  void set_is_dynamic(bool is_dynamic) { is_dynamic_ = is_dynamic; }
  bool vm_compiled() const { return vm_compiled_; }
  void set_vm_compiled(bool vm_compiled) { vm_compiled_ = vm_compiled; }
  bool ms_function_flag() const { return ms_function_flag_; }
  void set_ms_function_flag(bool ms_function_flag) { ms_function_flag_ = ms_function_flag; }
  bool need_compile_graph() const { return need_compile_graph_; }
  void set_need_compile_graph(bool need_compile_graph) { need_compile_graph_ = need_compile_graph; }
  bool forward_already_run() const { return forward_already_run_; }
  void set_forward_already_run(bool set_forward_already_run) { forward_already_run_ = set_forward_already_run; }
  pipeline::ResourcePtr resource() { return resource_; }
  FuncGraphPtr df_builder() { return df_builder_; }
  size_t op_num() const { return op_num_; }
  void set_op_num(size_t op_num) { op_num_ = op_num; }
  std::string &cell_id() { return cell_id_; }
  std::string &input_args_id() { return input_args_id_; }
  std::string &all_op_info() { return all_op_info_; }
  void set_input_args_id(const std::string &input_args_id) { input_args_id_ = std::move(input_args_id); }
  std::unordered_set<std::string> &sub_cell_list() { return sub_cell_list_; }
  bool IsSubCell(const std::string &cell_id) const;
  OrderedMap<FuncGraphPtr, GraphInfoPtr> &graph_info_map() { return graph_info_map_; }
  OpInfoWithTensorId &op_info_with_tensor_id() { return op_info_with_tensor_id_; }
  TensorIdWithTensorObject &tensor_id_with_tensor_object() { return tensor_id_with_tensor_object_; }
  ad::KPynativeCellPtr k_pynative_cell_ptr() const { return k_pynative_cell_ptr_; }
  void set_k_pynative_cell_ptr(const ad::KPynativeCellPtr &k_pynative_cell_ptr) {
    k_pynative_cell_ptr_ = k_pynative_cell_ptr;
  }
  const MsFunctionGradCache &ms_function_grad_cache() const { return ms_function_grad_cache_; }
  void set_ms_function_grad_cache(const std::string &graph_phase, const FuncGraphPtr &func_graph,
                                  const FuncGraphPtr &grad_graph) {
    ms_function_grad_cache_[graph_phase] = std::make_pair(func_graph, grad_graph);
  }
  void ClearDeviceMemory();
  void Clear();

 private:
  bool is_topest_{false};
  bool is_dynamic_{false};
  bool vm_compiled_{false};
  bool ms_function_flag_{false};
  bool is_init_kpynative_{false};
  bool forward_already_run_{false};
  bool need_compile_graph_{false};
  size_t op_num_{0};
  size_t grad_order_{0};
  pipeline::ResourcePtr resource_{nullptr};
  FuncGraphPtr df_builder_{nullptr};
  ad::KPynativeCellPtr k_pynative_cell_ptr_{nullptr};
  std::string cell_id_;
  std::string input_args_id_;
  std::string all_op_info_;
  OrderedMap<FuncGraphPtr, GraphInfoPtr> graph_info_map_;
  std::unordered_set<std::string> sub_cell_list_;
  OpInfoWithTensorId op_info_with_tensor_id_;
  TensorIdWithTensorObject tensor_id_with_tensor_object_;
  MsFunctionGradCache ms_function_grad_cache_;
};
using TopCellInfoPtr = std::shared_ptr<TopCellInfo>;

class ForwardExecutor;
using ForwardExecutorPtr = std::shared_ptr<ForwardExecutor>;
using ForwardExecutorWeakPtr = std::weak_ptr<ForwardExecutor>;

class GradExecutor;
using GradExecutorPtr = std::shared_ptr<GradExecutor>;
using GradExecutorWeakPtr = std::weak_ptr<GradExecutor>;

class GradExecutor {
 public:
  GradExecutor() = default;
  ~GradExecutor() = default;
  explicit GradExecutor(const ForwardExecutorPtr &forward_executor = nullptr)
      : forward_executor_(ForwardExecutorWeakPtr(forward_executor)) {}

  std::function<void(py::object *, const py::object &, const py::args &)> InitGraph = [this](auto &&PH1, auto &&PH2,
                                                                                             auto &&PH3) {
    NewGraphInner(std::forward<decltype(PH1)>(PH1), std::forward<decltype(PH2)>(PH2), std::forward<decltype(PH3)>(PH3));
  };
  std::function<void(py::object *, const py::object &, const py::object &, const py::args &)> LinkGraph =
    [this](auto &&PH1, auto &&PH2, auto &&PH3, auto &&PH4) {
      EndGraphInner(std::forward<decltype(PH1)>(PH1), std::forward<decltype(PH2)>(PH2),
                    std::forward<decltype(PH3)>(PH3), std::forward<decltype(PH4)>(PH4));
    };
  std::function<void(py::object *, const prim::GradOperationPtr &, const py::object &, const py::object &,
                     const py::args &)>
    GradGraph = [this](auto &&PH1, auto &&PH2, auto &&PH3, auto &&PH4, auto &&PH5) {
      GradNetInner(std::forward<decltype(PH1)>(PH1), std::forward<decltype(PH2)>(PH2), std::forward<decltype(PH3)>(PH3),
                   std::forward<decltype(PH4)>(PH4), std::forward<decltype(PH5)>(PH5));
    };
  std::function<void(py::object *, const py::object &, const py::tuple &)> RunGraph = [this](auto &&PH1, auto &&PH2,
                                                                                             auto &&PH3) {
    RunGradGraph(std::forward<decltype(PH1)>(PH1), std::forward<decltype(PH2)>(PH2), std::forward<decltype(PH3)>(PH3));
  };

  FuncGraphPtr curr_g() const;
  TopCellInfoPtr top_cell() const;
  void CheckNeedCompileGraph();
  TopCellInfoPtr GetTopCell(const string &cell_id) const;
  bool need_renormalize() const { return need_renormalize_; }
  void set_top_cell(TopCellInfoPtr top_cell) { top_cell_ = std::move(top_cell); }
  bool grad_flag() const { return grad_flag_; }
  void set_grad_flag(bool flag) { grad_flag_ = flag; }
  void set_graph_phase(const std::string &graph_phase) { graph_phase_ = graph_phase; }
  bool in_cell_with_custom_bprop_() const { return custom_bprop_cell_count_ > 0; }
  AnfNodePtr GetInput(const py::object &obj, bool op_mask);
  std::string GetCellId(const py::object &obj, const py::args &args);
  void RecordGradOpInfo(const OpExecInfoPtr &op_exec_info, const py::object &ret);
  bool need_construct_graph() const { return !cell_stack_.empty() && grad_flag_; }
  void SaveOutputNodeMap(const std::string &obj_id, const py::object &out_real, const AnfNodePtr &cnode);
  void DoOpGrad(const OpExecInfoPtr &op_exec_info, const AnfNodePtr &node, const py::object &op_out);
  void MakeAdjointForMsFunction(const FuncGraphPtr &ms_func_graph, const FuncGraphPtr &fprop_g, const py::object &out,
                                const py::args &args, const std::string &graph_phase);
  void MakeCNodeForMsFunction(const FuncGraphPtr &ms_func_graph, const py::args &args,
                              const OpExecInfoPtr &op_exec_info, ValuePtrList *input_values,
                              CNodePtr *ms_function_cnode);
  void UpdateForwardTensorInfoInBpropGraph(const OpExecInfoPtr &op_exec_info, const py::object &out_real);
  void SaveForwardTensorInfoInBpropGraph(const pipeline::ResourcePtr &resource) const;
  py::object CheckGraph(const py::object &cell, const py::args &args);
  void RunGradGraph(py::object *ret, const py::object &cell, const py::tuple &args);
  void EraseTopCellFromTopCellList(const TopCellInfoPtr &top_cell);
  void GradMsFunction(const py::object &out, const py::args &args);
  void ClearGrad(const py::object &cell, const py::args &args);
  void ClearRes();
  void ClearCellRes(const std::string &cell_id = "");

 private:
  ForwardExecutorPtr forward() const;
  // Higher derivative
  bool IsNestedGrad() const;
  void SwitchTopcell();
  size_t GetHighOrderStackSize() const { return high_order_stack_.size(); }
  void MakeNestedCnode(const py::object &cell, const std::string &cell_id, const py::args &forward_args,
                       const pipeline::ResourcePtr &resource, const py::object &out);
  void PushCellStack(const std::string &cell_id);
  void PopCellStack();
  void PushHighOrderGraphStack(const TopCellInfoPtr &top_cell);
  TopCellInfoPtr PopHighOrderGraphStack();
  // Manage information of top cell.
  FuncGraphPtr GetDfbuilder(const std::string &cell_id = "");
  pipeline::ResourcePtr GetResource(const std::string &cell_id = "");
  void HandleInputArgsForTopCell(const py::args &args, bool is_bprop_top);
  void InitResourceAndDfBuilder(const std::string &cell_id, const py::args &args);
  void MakeNewTopGraph(const string &cell_id, const py::args &args, bool is_topest);
  void UpdateTopCellInfo(bool forward_already_run, bool need_compile_graph, bool vm_compiled);
  // Manage resource when run grad process.
  bool IsBpropGraph(const std::string &cell_id);
  bool IsCellObjIdEq(const std::string &l_cell_id, const std::string &r_cell_id);
  void DumpGraphIR(const std::string &filename, const FuncGraphPtr &graph);
  void NewGraphInner(py::object *ret, const py::object &cell, const py::args &args);
  void EndGraphInner(py::object *ret, const py::object &cell, const py::object &out, const py::args &args);
  void DoGradForCustomBprop(const py::object &cell, const py::object &out, const py::args &args);
  std::string GetGradCellId(bool has_sens, const py::object &cell, const py::args &args,
                            py::args *forward_args = nullptr);
  void GradNetInner(py::object *ret, const prim::GradOperationPtr &grad, const py::object &cell,
                    const py::object &weights, const py::args &args);
  FuncGraphPtr GetBpropGraph(const prim::GradOperationPtr &grad, const py::object &cell,
                             const std::vector<AnfNodePtr> &weights, size_t arg_size, const py::args &args);
  std::vector<AnfNodePtr> GetWeightsArgs(const py::object &weights, const FuncGraphPtr &df_builder);
  abstract::AbstractBasePtrList GetArgsSpec(const py::args &args, const FuncGraphPtr &bprop_graph);
  // Manage resource for construct forward graph.
  std::string &graph_phase() { return graph_phase_; }
  AnfNodePtr GetObjNode(const py::object &obj, const std::string &obj_id);
  AnfNodePtr MakeValueNode(const py::object &obj, const std::string &obj_id);
  void SetTupleItemArgsToGraphInfoMap(const FuncGraphPtr &g, const py::object &id, const AnfNodePtr &node,
                                      const std::vector<int64_t> &index_sequence, bool is_param = false);
  void SetTupleArgsToGraphInfoMap(const FuncGraphPtr &g, const py::object &args, const AnfNodePtr &node,
                                  bool is_param = false);
  void SetParamNodeMapInGraphInfoMap(const FuncGraphPtr &g, const std::string &id, const ParameterPtr &param) {
    top_cell()->graph_info_map()[g]->params[id] = param;
  }
  void SetNodeMapInGraphInfoMap(const FuncGraphPtr &g, const std::string &id, const AnfNodePtr &node,
                                int64_t index = -1) {
    top_cell()->graph_info_map()[g]->node_map[id] = std::make_pair(node, std::vector<int64_t>{index});
  }
  void SetNodeMapInGraphInfoMap(const FuncGraphPtr &g, const std::string &id, const AnfNodePtr &node,
                                const std::vector<int64_t> &index) {
    top_cell()->graph_info_map()[g]->node_map[id] = std::make_pair(node, index);
  }
  void CreateMakeTupleNodeForMultiOut(const std::string &cell_id, const FuncGraphPtr &curr_g, const py::object &out,
                                      const std::string &out_id);

 private:
  bool grad_flag_{false};
  bool need_renormalize_{false};
  bool grad_is_running_{false};
  int custom_bprop_cell_count_{0};
  size_t grad_order_{0};

  // The graph phase is used to obtain backend graph that is complied by ms_function
  std::string graph_phase_;
  // Only set in high grad
  FuncGraphPtr curr_g_{nullptr};
  // For clear pre top res
  TopCellInfoPtr top_cell_{nullptr};
  // Records forwrad cell, the bottom is top cell
  std::stack<std::string> cell_stack_;
  // For high grad of bprop
  std::stack<std::pair<std::string, bool>> bprop_grad_stack_;
  std::vector<std::string> bprop_cell_list_;
  // For high grad order
  std::stack<std::pair<FuncGraphPtr, TopCellInfoPtr>> high_order_stack_;
  // Use vector for keep order
  std::vector<TopCellInfoPtr> top_cell_list_;
  // Record all top cell which has been ran
  std::map<CellId, TopCellInfoPtr> already_run_top_cell_;
  // Use vector for keep order
  ForwardExecutorWeakPtr forward_executor_;
};

class ForwardExecutor {
 public:
  ForwardExecutor() = default;
  ~ForwardExecutor() = default;

  std::function<void(py::object *, const OpExecInfoPtr &)> RunOpS = [this](auto &&PH1, auto &&PH2) {
    RunOpInner(std::forward<decltype(PH1)>(PH1), std::forward<decltype(PH2)>(PH2));
  };

  void RunOpInner(py::object *ret, const OpExecInfoPtr &op_exec_info);
  OpExecInfoPtr GenerateOpExecInfo(const py::args &args);
  void set_grad_executor(const GradExecutorPtr &grad_executor) { grad_executor_ = GradExecutorWeakPtr(grad_executor); }
  std::unordered_map<std::string, abstract::AbstractBasePtr> &node_abs_map() { return node_abs_map_; }
  void ClearRes();
  AnfNodePtr ConstructForwardGraph(const OpExecInfoPtr &op_exec_info);

 private:
  GradExecutorPtr grad() const;
  MsBackendPolicy InitEnv(const OpExecInfoPtr &op_exec_info);
  py::tuple RunOpWithInitBackendPolicy(const OpExecInfoPtr &op_exec_info);
  void RunMixedPrecisionCastOp(const OpExecInfoPtr &op_exec_info, py::object *ret);
  py::object RunOpInVM(const OpExecInfoPtr &op_exec_info, PynativeStatusCode *status);
  py::object RunOpInMs(const OpExecInfoPtr &op_exec_info, PynativeStatusCode *status);
  py::object RunOpWithBackendPolicy(MsBackendPolicy backend_policy, const OpExecInfoPtr &op_exec_info,
                                    PynativeStatusCode *status);
  void GetInputsArgsSpec(const OpExecInfoPtr &op_exec_info, abstract::AbstractBasePtrList *args_spec_list);
  void GetOpOutputAbstract(const OpExecInfoPtr &op_exec_info, const abstract::AbstractBasePtrList &args_spec_list,
                           bool *prim_cache_hit);
  void GetOpOutput(const OpExecInfoPtr &op_exec_info, const abstract::AbstractBasePtrList &args_spec_list,
                   const AnfNodePtr &CNode, bool prim_cache_hit, py::object *ret);
  // Mix precision and Implicit transform
  void SetCastForInputs(const OpExecInfoPtr &op_exec_info);
  void SetParameterMixPrecisionCast(const OpExecInfoPtr &op_exec_info);
  void SetImplicitCast(const OpExecInfoPtr &op_exec_info);
  py::object DoParamMixPrecisionCast(bool *is_cast, const py::object &obj, const std::string &op_name, size_t index);
  py::object DoParamMixPrecisionCastTuple(bool *is_cast, const py::tuple &tuple, const std::string &op_name,
                                          size_t index);
  py::object DoAutoCast(const py::object &arg, const TypeId &type_id, const std::string &op_name, size_t index);
  void DoSignatrueCast(const PrimitivePyPtr &prim, const std::map<SignatureEnumDType, TypeId> &dst_type,
                       const std::vector<SignatureEnumDType> &dtypes, const OpExecInfoPtr &op_exec_info);

 private:
  GradExecutorWeakPtr grad_executor_;
  PrimAbsCache prim_abs_list_;
  std::unordered_map<std::string, abstract::AbstractBasePtr> node_abs_map_;
};

class PynativeExecutor : public std::enable_shared_from_this<PynativeExecutor> {
 public:
  static std::shared_ptr<PynativeExecutor> GetInstance() {
    std::lock_guard<std::mutex> i_lock(instance_lock_);
    if (executor_ == nullptr) {
      executor_ = std::shared_ptr<PynativeExecutor>(new (std::nothrow) PynativeExecutor());
      forward_executor_ = std::make_shared<ForwardExecutor>();
      grad_executor_ = std::make_shared<GradExecutor>(forward_executor_);
      forward_executor_->set_grad_executor(grad_executor_);
    }
    return executor_;
  }
  ~PynativeExecutor() = default;
  PynativeExecutor(const PynativeExecutor &) = delete;
  PynativeExecutor &operator=(const PynativeExecutor &) = delete;

  void EnterConstruct(const py::object &cell);
  void LeaveConstruct(const py::object &cell);
  GradExecutorPtr grad_executor() const;
  ForwardExecutorPtr forward_executor() const;

  void set_grad_flag(bool flag);
  void set_graph_phase(const std::string &graph_phase);
  void GradMsFunction(const py::object &out, const py::args &args);
  void NewGraph(const py::object &cell, const py::args &args);
  void EndGraph(const py::object &cell, const py::object &out, const py::args &args);
  void GradNet(const prim::GradOperationPtr &grad, const py::object &cell, const py::object &weights,
               const py::args &args);
  py::object CheckGraph(const py::object &cell, const py::args &args);
  py::object CheckAlreadyRun(const py::object &cell, const py::args &args);
  py::object Run(const py::object &cell, const py::tuple &args);

  // Used by graph clean
  // Cell destruct will call
  void ClearCell(const std::string &cell_id);
  void ClearGrad(const py::object &cell, const py::args &args);
  // Abnormal existed
  void ClearRes();
  // Sync stream
  void Sync();

 private:
  PynativeExecutor() = default;

  static std::shared_ptr<PynativeExecutor> executor_;
  static std::mutex instance_lock_;
  static ForwardExecutorPtr forward_executor_;
  static GradExecutorPtr grad_executor_;
  // The pointer of top python Cell object, which is always the network(inherit class Cell) ran in python test script,
  // such as Resnet50(Cell),LeNet(Cell).This pointer is used to distinguish temporary primitives from global
  // primitives to control memory release. Global primitives are always created in top cell's '__init__' function and
  // temporary primitives are always created in other place.Temporary primitives will be released after executing top
  // cell's 'construct' function but global primitives will not.
  PyObject *py_top_cell_{nullptr};
};

using PynativeExecutorPtr = std::shared_ptr<PynativeExecutor>;
}  // namespace mindspore::pynative

#endif  // MINDSPORE_CCSRC_PIPELINE_PYNATIVE_PYNATIVE_EXECUTE_H_
