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

#include "include/registry/pass_registry.h"
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "tools/converter/registry/pass_content.h"
#include "src/common/log_adapter.h"

namespace mindspore {
namespace lite {
namespace {
std::map<std::string, opt::PassPtr> pass_store_room;
std::map<registry::PassPosition, std::vector<std::string>> external_assigned_passes;
std::mutex pass_mutex;
void RegPass(const std::string &pass_name, const opt::PassPtr &pass) {
  if (pass == nullptr) {
    MS_LOG(ERROR) << "pass is nullptr.";
    return;
  }
  std::unique_lock<std::mutex> lock(pass_mutex);
  pass_store_room[pass_name] = pass;
}
}  // namespace

registry::PassRegistry::PassRegistry(const std::string &pass_name, const opt::PassPtr &pass) {
  RegPass(pass_name, pass);
}

registry::PassRegistry::PassRegistry(PassPosition position, const std::vector<std::string> &assigned) {
  std::unique_lock<std::mutex> lock(pass_mutex);
  external_assigned_passes[position] = assigned;
}

std::map<std::string, opt::PassPtr> &PassStoreRoomInfo() { return pass_store_room; }

std::map<registry::PassPosition, std::vector<std::string>> &ExternalAssignedPassesInfo() {
  return external_assigned_passes;
}
}  // namespace lite
}  // namespace mindspore
