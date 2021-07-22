/**
 * Copyright 2019-2021 Huawei Technologies Co., Ltd
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

#include "debug/trace.h"

#include <iostream>
#include <fstream>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <sstream>
#include <utility>
#include <stack>
#include <algorithm>

#include "ir/meta_func_graph.h"
#include "ir/graph_utils.h"
#include "frontend/operator/composite/composite.h"
#include "ir/tensor.h"
#include "debug/anf_ir_utils.h"
#include "debug/common.h"
#include "pipeline/jit/static_analysis/evaluator.h"
#include "pipeline/jit/static_analysis/async_eval_result.h"
#include "utils/log_adapter.h"
#include "abstract/abstract_value.h"

namespace mindspore {
// namespace to support debug trace information
namespace trace {
using abstract::AbstractBasePtr;
using abstract::AnalysisContextPtr;
using abstract::AnalysisEnginePtr;
using abstract::AnfNodeConfigPtr;

std::string GetAbstractStr(const abstract::AbstractBasePtr &abs) {
  if (abs == nullptr) {
    return "NullAbstract";
  }
  auto shape = abs->BuildShape()->cast<abstract::ShapePtr>();
  TypePtr type = abs->BuildType();
  std::ostringstream oss;
  if ((shape != nullptr) && (type != nullptr)) {
    oss << type->DumpText() << shape->DumpText();
  } else if (type != nullptr) {
    oss << type->DumpText();
  } else {
    oss << "Undefined";
  }
  return oss.str();
}

std::string GetGraphParamString(const FuncGraphPtr &graph, abstract::AbstractBasePtrList args_spec_list) {
  MS_EXCEPTION_IF_NULL(graph);
  std::ostringstream oss;
  oss << "graph:" << graph->ToString() << " with args[";
  auto params = graph->parameters();
  if (params.size() < args_spec_list.size()) {
    MS_EXCEPTION(TypeError) << "The size of parameters less than args_spec_list's size.";
  }
  for (size_t i = 0; i < args_spec_list.size(); i++) {
    auto parameter = params[i];
    MS_EXCEPTION_IF_NULL(parameter);
    oss << parameter->ToString() << ":<" << GetAbstractStr(args_spec_list[i]) << ">,";
  }
  oss << "]";
  oss << GetDebugInfo(graph->debug_info(), kSourceLineTipDiscard);
  return oss.str();
}

void DumpInferStack(std::ostringstream &oss) {
  auto &graph_stack = GetCurrenGraphEvalStack();
  if (graph_stack.empty()) {
    return;
  }
  std::vector<std::pair<abstract::AnalysisContextPtr, abstract::AnfNodeConfigPtr>> infer_vec;
  while (!graph_stack.empty()) {
    auto top = graph_stack.top();
    infer_vec.push_back(top);
    graph_stack.pop();
  }
  std::reverse(infer_vec.begin(), infer_vec.end());
  int index = 0;
  for (const auto &item : infer_vec) {
    auto context = item.first;
    if (context == nullptr) {
      MS_LOG(EXCEPTION) << "DumpInferStack failed, got null graph context";
    }
    auto graph = context->func_graph();
    if (graph == nullptr) {  // Top context.
      continue;
    }
    auto args_spec_list = context->args_spec_list();
    if (graph->parameters().size() < args_spec_list.size()) {
      continue;
    }
    oss << "    #" << index++ << " " << GetGraphParamString(graph, args_spec_list) << "\n";
  }
}

void TraceGraphEval() {
  auto &graph_stack = GetCurrenGraphEvalStack();
  if (graph_stack.empty()) {
    MS_LOG(INFO) << "Length of analysis graph stack is empty.";
    return;
  }
  MS_LOG(ERROR) << "\n*******************************graph evaluate stack**********************************";
  std::ostringstream oss;
  oss << std::endl;
  DumpInferStack(oss);
  MS_LOG(ERROR) << oss.str();
  MS_LOG(ERROR) << "\n*************************************************************************************";
}

class AnalyzeFailExporter : public AnfExporter {
 public:
  AnalyzeFailExporter() : AnfExporter(true, false) {}
  ~AnalyzeFailExporter() override = default;

  bool ExportFuncGraph(const std::string &filename, const std::vector<abstract::AnfNodeConfigPtr> &node_config_stack);

 private:
  void OutputCNode(std::ofstream &ofs, const CNodePtr &cnode, const FuncGraphPtr &func_graph, int *idx,
                   std::map<AnfNodePtr, int> *const apply_map) override;
  std::string GetNodeType(const AnfNodePtr &nd) override;
  AbstractBasePtr GetNodeAbstract(const AnfNodePtr &nd);
  AnfNodeConfigPtr GetFordwardConfig(const AnfNodeConfigPtr &cfg);
  void ProcessFuncGraphCall(const CNodePtr &node, std::string *const op_comment);
  void OutputStatementComment(std::ofstream &ofs, const CNodePtr &node);
  std::unordered_map<FuncGraphPtr, TaggedNodeMap> CreateTaggedNodeMap(
    const std::vector<abstract::AnfNodeConfigPtr> &node_config_stack);

  AnalysisContextPtr current_context_ = nullptr;
  AnalysisEnginePtr engine_ = nullptr;
};

std::unordered_map<FuncGraphPtr, TaggedNodeMap> AnalyzeFailExporter::CreateTaggedNodeMap(
  const std::vector<abstract::AnfNodeConfigPtr> &node_config_stack) {
  std::unordered_set<abstract::AnfNodeConfigPtr> forwarded_configs;  // Check if config. is forwarded.
  std::unordered_map<FuncGraphPtr, TaggedNodeMap> tagged_func_graphs;
  size_t index = 0;
  for (auto &node_config : node_config_stack) {
    MS_EXCEPTION_IF_NULL(node_config);

    // Record new config in set.
    auto new_config = GetFordwardConfig(node_config);
    if (new_config != node_config) {
      MS_LOG(DEBUG) << "The node_config is forwarded, old config: " << node_config->ToString()
                    << ", new_config: " << new_config->ToString();
      forwarded_configs.emplace(new_config);
    }

    // Ignore the new config.
    if (forwarded_configs.find(node_config) != forwarded_configs.end()) {
      continue;
    }

    auto fg = node_config->func_graph();
    MS_EXCEPTION_IF_NULL(fg);
    auto node = node_config->node();
    tagged_func_graphs[fg][node] = index;
    index++;
  }
  return tagged_func_graphs;
}

bool OutputAnalyzedGraphWithType(const string &file_path) {
  AnalyzeFailExporter exporter;
  return exporter.ExportFuncGraph(file_path, GetCNodeDebugStack());
}

std::string AnalyzeFailExporter::GetNodeType(const AnfNodePtr &node) {
  if (current_context_ == nullptr) {
    return AnfExporter::GetNodeType(node);
  }

  MS_EXCEPTION_IF_NULL(engine_);
  try {
    FuncGraphPtr dummy_call_func_graph = nullptr;
    auto cfg = engine_->MakeConfig(node, current_context_, dummy_call_func_graph);
    auto res = abstract::AnalysisResultCacheMgr::GetInstance().GetValue(cfg);
    if (res != nullptr) {
      return GetAbstractStr(res->abstract());
    }
  } catch (const std::exception &e) {
    MS_LOG(INFO) << "Exception: " << e.what();
  }
  return "Undefined";
}

AbstractBasePtr AnalyzeFailExporter::GetNodeAbstract(const AnfNodePtr &node) {
  if (current_context_ == nullptr) {
    return nullptr;
  }
  MS_EXCEPTION_IF_NULL(engine_);
  try {
    FuncGraphPtr dummy_call_func_graph = nullptr;
    auto cfg = engine_->MakeConfig(node, current_context_, dummy_call_func_graph);
    auto res = abstract::AnalysisResultCacheMgr::GetInstance().GetValue(cfg);
    return res == nullptr ? nullptr : res->abstract();
  } catch (const std::exception &e) {
    MS_LOG(INFO) << "Exception: " << e.what();
  }
  return nullptr;
}

AnfNodeConfigPtr AnalyzeFailExporter::GetFordwardConfig(const AnfNodeConfigPtr &cfg) {
  AnfNodeConfigPtr cur_cfg = cfg;
  auto iter = engine_->anfnode_config_map().find(cur_cfg);
  while (iter != engine_->anfnode_config_map().end()) {
    auto node = cur_cfg->node();
    cur_cfg = iter->second;
    MS_LOG(DEBUG) << "Get forword node: " << node << "[" << node->DebugString() << "] --> " << cur_cfg->node() << "["
                  << cur_cfg->node()->DebugString() << "]";
    iter = engine_->anfnode_config_map().find(cur_cfg);
  }
  return cur_cfg;
}

void AnalyzeFailExporter::ProcessFuncGraphCall(const CNodePtr &node, std::string *const op_comment) {
  if (node == nullptr) {
    MS_LOG(ERROR) << "Node is nullptr";
    return;
  }
  CNodePtr cnode = nullptr;
  try {
    FuncGraphPtr dummy_call_func_graph = nullptr;
    auto cfg = engine_->MakeConfig(node, current_context_, dummy_call_func_graph);
    cfg = GetFordwardConfig(cfg);
    cnode = dyn_cast<CNode>(cfg->node());
  } catch (const std::exception &e) {
    MS_LOG(INFO) << "Exception: " << e.what();
  }
  if (cnode == nullptr) {
    MS_LOG(INFO) << "CNode is nullptr";
    return;
  }

  const auto &inputs = cnode->inputs();
  for (size_t i = 0; i < inputs.size(); ++i) {
    auto op_abs = GetNodeAbstract(inputs[i]);
    if (op_abs == nullptr) {
      MS_LOG(DEBUG) << "Abstract of inputs[" << i << "] of cnode " << cnode->ToString() << "  is nullptr";
      continue;
    }

    if (!op_abs->isa<abstract::FuncGraphAbstractClosure>() && !op_abs->isa<abstract::MetaFuncGraphAbstractClosure>()) {
      MS_LOG(DEBUG) << "Inputs[" << i << "] of cnode " << cnode->ToString() << " is of type " << op_abs->type_name()
                    << ", not function, ignore it";
      // Get prototype of VirtualEvaluator for printing
      if (i == 0 && op_abs->isa<abstract::VirtualAbstractClosure>()) {
        auto func = dyn_cast<abstract::VirtualAbstractClosure>(op_abs);
        std::ostringstream oss;
        oss << "(";
        bool first_flag = false;
        for (const auto &arg : func->args_spec_list()) {
          if (!first_flag) {
            first_flag = true;
          } else {
            oss << ", ";
          }
          oss << GetAbstractStr(arg);
        }
        oss << ") -> " << GetAbstractStr(func->output()) << " ";
        *op_comment = oss.str();
      }
    }
  }
}

void AnalyzeFailExporter::OutputStatementComment(std::ofstream &ofs, const CNodePtr &node) {
  if (node == nullptr) {
    return;
  }

  // Output type of each input argument
  auto &inputs = node->inputs();
  if (inputs.size() > 1) {
    ofs << "    #(";
    for (size_t i = 1; i < inputs.size(); ++i) {
      if (i != 1) {
        ofs << ", ";
      }
      AnfNodePtr arg = inputs[i];
      ofs << GetNodeType(arg);
    }
    ofs << ")";
  }
  // Output other comment, map the graph name to original representation(containing unicode character)
  std::ostringstream comment;
  comment << "    #";
  bool has_comment = false;
  for (size_t i = 0; i < inputs.size(); ++i) {
    AnfNodePtr arg = inputs[i];
    if (!IsValueNode<FuncGraph>(arg)) {
      continue;
    }
    if (!has_comment) {
      has_comment = true;
    } else {
      comment << ",";
    }
    FuncGraphPtr fg = GetValueNode<FuncGraphPtr>(arg);
    std::string func_graph_id = fg->debug_info()->get_id();
    comment << " fg_" << func_graph_id << "=" << fg->ToString();
  }
  if (has_comment) {
    ofs << comment.str();
  }
  ofs << " #scope: " << node->scope()->name();
}

void AnalyzeFailExporter::OutputCNode(std::ofstream &ofs, const CNodePtr &cnode, const FuncGraphPtr &func_graph,
                                      int *idx, std::map<AnfNodePtr, int> *const apply_map) {
  OutputCNodeText(ofs, cnode, func_graph, idx, apply_map);
  // Process function graph call
  std::string op_comment;
  ProcessFuncGraphCall(cnode, &op_comment);
  if (!op_comment.empty()) {
    auto &inputs = cnode->inputs();
    ofs << "    #" << GetAnfNodeText(func_graph, inputs[0], *apply_map) << ".prototype = " << op_comment;
  }
  // Output comment
  OutputStatementComment(ofs, cnode);
  ofs << "\n";
}

bool AnalyzeFailExporter::ExportFuncGraph(const std::string &filename,
                                          const std::vector<abstract::AnfNodeConfigPtr> &node_config_stack) {
  if (node_config_stack.empty()) {
    MS_LOG(DEBUG) << "Node configs is empty";
    return false;
  }
  std::ofstream ofs(filename);
  if (!ofs.is_open()) {
    MS_LOG(ERROR) << "Open file '" << filename << "' failed!";
    return false;
  }

  if (engine_ == nullptr) {
    engine_ = node_config_stack.front()->engine();
  }

  auto tagged_func_graphs = CreateTaggedNodeMap(node_config_stack);
  std::unordered_set<FuncGraphPtr> printed_func_graphs;  // Check if func graph has been printed.
  // Output graph on the analysis stack
  for (const auto &node_config : node_config_stack) {
    auto fg = node_config->func_graph();
    MS_LOG(INFO) << "Node: " << node_config->node()->DebugString()
                 << ", FV: " << (node_config->func_graph() != node_config->context()->func_graph())
                 << ", calling func graph: " << node_config->func_graph()->ToString()
                 << ", context func graph: " << node_config->context()->func_graph()->ToString();
    if (fg == nullptr) {
      MS_LOG(ERROR) << "FuncGraph is null, context: " << node_config->ToString();
      continue;
    }
    if (printed_func_graphs.find(fg) != printed_func_graphs.end()) {
      continue;
    }
    (void)printed_func_graphs.emplace(fg);

    current_context_ = node_config->context();  // Set current context.
    ExportOneFuncGraph(ofs, fg, tagged_func_graphs[fg]);
    ofs << "\n\n";
  }

  ofs << "#===============================================================================\n";
  ofs << "# num of function graphs in stack: ";
  auto ignored_num = (node_config_stack.size() - printed_func_graphs.size());
  if (ignored_num == 0) {
    ofs << node_config_stack.size() << "\n";
  } else {
    ofs << printed_func_graphs.size() << "/" << node_config_stack.size() << " (Ignored " << ignored_num
        << " internal frames).\n";
  }
  ofs.close();
  return true;
}

void GetEvalStackInfo(std::ostringstream &oss) {
  MS_LOG(INFO) << "Get graph analysis information begin";
  auto stack = GetCNodeDebugStack();
  if (stack.empty()) {
    MS_LOG(INFO) << "Length of analysis information stack is empty.";
    return;
  }
  static int fileNumber = 0;
  string file_name = "analyze_fail_" + std::to_string(fileNumber++) + ".dat";
  auto ms_om_path = common::GetEnv("MS_OM_PATH");
  if (!ms_om_path.empty()) {
    auto path = ms_om_path + "/" + file_name;
    auto realpath = Common::GetRealPath(path);
    if (!realpath.has_value()) {
      MS_EXCEPTION(ValueError) << "Get real path failed. path=" << path;
    }
    file_name = realpath.value();
  }

  auto ret = OutputAnalyzedGraphWithType(file_name);
  oss << "\nThe function call stack";
  if (ret) {
    oss << " (See file '" << file_name << "' for more details)";
  }
  oss << ":\n";

  int index = 0;
  std::string last_location_info = "";
  for (size_t i = 0; i < stack.size(); ++i) {
    auto node_config = stack[i];

    auto cnode = dyn_cast<CNode>(node_config->node());
    if (cnode == nullptr) {
      MS_LOG(DEBUG) << "CNode of elements[" << i << "] is nullptr.";
      continue;
    }

    auto debug_info = cnode->debug_info();
    auto this_location_info = trace::GetDebugInfo(debug_info, std::string(""));
    if (this_location_info.empty() || this_location_info == last_location_info) {
      continue;
    }

    last_location_info = this_location_info;
    oss << "# " << index++ << " " << this_location_info;
  }

  stack.clear();
  MS_LOG(INFO) << "Get graph analysis information *end*";
}

// Trace the graph evaluator stack
thread_local static std::stack<std::pair<abstract::AnalysisContextPtr, abstract::AnfNodeConfigPtr>> graph_infer_stack;
// Trace the cnode infer debug info
thread_local static std::vector<abstract::AnfNodeConfigPtr> cnode_debug_stack{};

void TraceGraphEvalEnter(const abstract::AnalysisContextPtr &context, const abstract::AnfNodeConfigPtr &node) {
  if (context == nullptr) {
    MS_LOG(EXCEPTION) << "GraphInferEnter got null context";
  }
  (void)graph_infer_stack.emplace(std::pair<abstract::AnalysisContextPtr, abstract::AnfNodeConfigPtr>(context, node));
}

void TraceGraphEvalLeave(const abstract::AnalysisContextPtr &context) {
  if (context == nullptr || graph_infer_stack.empty()) {
    MS_LOG(EXCEPTION) << "The context is null, or call stack is empty.";
  }
  if (context != graph_infer_stack.top().first) {
    MS_LOG(EXCEPTION) << "Different context: " << context->func_graph()->ToString() << ", "
                      << graph_infer_stack.top().first->func_graph()->ToString();
  }
  graph_infer_stack.pop();
}

void TraceEvalCNodeEnter(const abstract::AnfNodeConfigPtr &node_config) { cnode_debug_stack.push_back(node_config); }

void TraceEvalCNodeLeave() { cnode_debug_stack.pop_back(); }

std::vector<abstract::AnfNodeConfigPtr> &GetCNodeDebugStack() { return cnode_debug_stack; }

std::stack<std::pair<abstract::AnalysisContextPtr, abstract::AnfNodeConfigPtr>> &GetCurrenGraphEvalStack() {
  return graph_infer_stack;
}

void ClearTraceStack() {
  while (!graph_infer_stack.empty()) {
    graph_infer_stack.pop();
  }
  cnode_debug_stack.clear();
}

// Register trace provider to LogWriter.
struct TraceProviderRegister {
  TraceProviderRegister() {
    LogWriter::set_trace_provider([](std::ostringstream &oss) {
      TraceGraphEval();
      std::ostringstream trace_info;
      GetEvalStackInfo(trace_info);
      if (trace_info.str().empty()) {
        DebugInfoPtr debug_info = TraceManager::GetParseOrResolveDebugInfo();
        if (debug_info != nullptr) {
          oss << "\n\n# " << trace::GetDebugInfo(debug_info);
        }
      } else {
        oss << trace_info.str();
      }
    });
  }
  ~TraceProviderRegister() = default;
} trace_provider_regsiter;

// Register trace cnode provider to AbstractBase.
struct TraceNodeProviderRegister {
  TraceNodeProviderRegister() {
    abstract::AbstractBase::set_trace_node_provider([](AnfNodePtr *node) {
      auto stack = GetCNodeDebugStack();
      if (!stack.empty()) {
        auto conf = stack.back();
        *node = conf->node();
      }
    });
  }
  ~TraceNodeProviderRegister() = default;
} trace_node_provider_regsiter;
}  // namespace trace
}  // namespace mindspore
