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

#ifndef MINDSPORE_CCSRC_ARMOUR_CIPHER_META_STORAGE_H
#define MINDSPORE_CCSRC_ARMOUR_CIPHER_META_STORAGE_H

#include <utility>
#include <algorithm>
#include <map>
#include <vector>
#include <string>
#include <memory>
#ifndef _WIN32
#include <gmp.h>
#endif
#include "proto/ps.pb.h"
#include "utils/log_adapter.h"
#include "armour/secure_protocol/secret_sharing.h"
#include "schema/fl_job_generated.h"
#include "schema/cipher_generated.h"
#include "ps/server/distributed_metadata_store.h"
#include "ps/server/common.h"

namespace mindspore {
namespace armour {

constexpr int SHARE_MAX_SIZE = 256;
constexpr int SECRET_MAX_LEN_DOUBLE = 66;

struct clientshare_str {
  std::string fl_id;
  std::vector<unsigned char> share;
  int index;
};

struct CipherPublicPara {
  int t;
  int g;
  unsigned char prime[PRIME_MAX_LEN];
  unsigned char p[SECRET_MAX_LEN];
  float dp_eps;
  float dp_delta;
  float dp_norm_clip;
  string encrypt_type;
};

class CipherMetaStorage {
 public:
  // Register the shared value involved in the security aggregation.
  void RegisterClass();

  // Register Prime.
  void RegisterPrime(const char *list_name, const std::string &prime);
  // Get tprime from shared server.
  bool GetPrimeFromServer(const char *list_name, unsigned char *prime);
  // Get client shares from shared server.
  void GetClientSharesFromServer(const char *list_name,
                                 std::map<std::string, std::vector<clientshare_str>> *clients_shares_list);
  // Get client list from shared server.
  void GetClientListFromServer(const char *list_name, std::vector<std::string> *clients_list);
  // Get client keys from shared server.
  void GetClientKeysFromServer(const char *list_name,
                               std::map<std::string, std::vector<std::vector<unsigned char>>> *clients_keys_list);
  // Get client noises from shared server.
  bool GetClientNoisesFromServer(const char *list_name, std::vector<float> *cur_public_noise);
  // Update client fl_id to shared server.
  bool UpdateClientToServer(const char *list_name, const std::string &fl_id);
  // Update client key to shared server.
  bool UpdateClientKeyToServer(const char *list_name, const std::string &fl_id,
                               const std::vector<std::vector<unsigned char>> &cur_public_key);
  // Update client noise to shared server.
  bool UpdateClientNoiseToServer(const char *list_name, const std::vector<float> &cur_public_noise);
  // Update client share to shared server.
  bool UpdateClientShareToServer(
    const char *list_name, const std::string &fl_id,
    const flatbuffers::Vector<flatbuffers::Offset<mindspore::schema::ClientShare>> *shares);
};
}  // namespace armour
}  // namespace mindspore

#endif  // MINDSPORE_CCSRC_ARMOUR_CIPHER_META_STORAGE_H
