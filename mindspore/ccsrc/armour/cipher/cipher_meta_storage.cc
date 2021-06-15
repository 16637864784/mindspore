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

#include "armour/cipher/cipher_meta_storage.h"

namespace mindspore {
namespace armour {

void CipherMetaStorage::GetClientSharesFromServer(
  const char *list_name, std::map<std::string, std::vector<clientshare_str>> *clients_shares_list) {
  const ps::PBMetadata &clients_shares_pb_out =
    ps::server::DistributedMetadataStore::GetInstance().GetMetadata(list_name);
  const ps::ClientShares &clients_shares_pb = clients_shares_pb_out.client_shares();
  auto iter = clients_shares_pb.client_secret_shares().begin();
  for (; iter != clients_shares_pb.client_secret_shares().end(); ++iter) {
    std::string fl_id = iter->first;
    const ps::SharesPb &shares_pb = iter->second;
    std::vector<clientshare_str> encrpted_shares_new;
    for (int index_shares = 0; index_shares < shares_pb.clientsharestrs_size(); ++index_shares) {
      const ps::ClientShareStr &client_share_str_pb = shares_pb.clientsharestrs(index_shares);
      clientshare_str new_clientshare;
      new_clientshare.fl_id = client_share_str_pb.fl_id();
      new_clientshare.index = client_share_str_pb.index();
      new_clientshare.share.assign(client_share_str_pb.share().begin(), client_share_str_pb.share().end());
      encrpted_shares_new.push_back(new_clientshare);
    }
    clients_shares_list->insert(std::pair<std::string, std::vector<clientshare_str>>(fl_id, encrpted_shares_new));
  }
}

void CipherMetaStorage::GetClientListFromServer(const char *list_name, std::vector<std::string> *clients_list) {
  const ps::PBMetadata &client_list_pb_out = ps::server::DistributedMetadataStore::GetInstance().GetMetadata(list_name);
  const ps::UpdateModelClientList &client_list_pb = client_list_pb_out.client_list();
  for (int i = 0; i < client_list_pb.fl_id_size(); ++i) {
    std::string fl_id = client_list_pb.fl_id(i);
    clients_list->push_back(fl_id);
  }
}

void CipherMetaStorage::GetClientKeysFromServer(
  const char *list_name, std::map<std::string, std::vector<std::vector<unsigned char>>> *clients_keys_list) {
  const ps::PBMetadata &clients_keys_pb_out =
    ps::server::DistributedMetadataStore::GetInstance().GetMetadata(list_name);
  const ps::ClientKeys &clients_keys_pb = clients_keys_pb_out.client_keys();

  for (auto iter = clients_keys_pb.client_keys().begin(); iter != clients_keys_pb.client_keys().end(); ++iter) {
    // const PairClientKeys & pair_client_keys_pb = clients_keys_pb.client_keys(i);
    std::string fl_id = iter->first;
    ps::KeysPb keys_pb = iter->second;
    std::vector<unsigned char> cpk(keys_pb.key(0).begin(), keys_pb.key(0).end());
    std::vector<unsigned char> spk(keys_pb.key(1).begin(), keys_pb.key(1).end());
    std::vector<std::vector<unsigned char>> cur_keys;
    cur_keys.push_back(cpk);
    cur_keys.push_back(spk);
    clients_keys_list->insert(std::pair<std::string, std::vector<std::vector<unsigned char>>>(fl_id, cur_keys));
  }
}

bool CipherMetaStorage::GetClientNoisesFromServer(const char *list_name, std::vector<float> *cur_public_noise) {
  const ps::PBMetadata &clients_noises_pb_out =
    ps::server::DistributedMetadataStore::GetInstance().GetMetadata(list_name);
  const ps::ClientNoises &clients_noises_pb = clients_noises_pb_out.client_noises();
  while (clients_noises_pb.has_one_client_noises() == false) {
    MS_LOG(INFO) << "GetClientNoisesFromServer NULL.";
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  cur_public_noise->assign(clients_noises_pb.one_client_noises().noise().begin(),
                           clients_noises_pb.one_client_noises().noise().end());
  return true;
}

bool CipherMetaStorage::GetPrimeFromServer(const char *list_name, unsigned char *prime) {
  const ps::PBMetadata &prime_pb_out = ps::server::DistributedMetadataStore::GetInstance().GetMetadata(list_name);
  auto &prime_list_pb = prime_pb_out.prime_list();
  if (prime_list_pb.prime_size() > 0 && prime_list_pb.prime(0).size() >= PRIME_MAX_LEN) {
    for (int i = 0; i < PRIME_MAX_LEN; i++) {
      prime[i] = static_cast<unsigned char>(prime_list_pb.prime(0)[i]);
    }
    return true;
  }
  return false;
}

bool CipherMetaStorage::UpdateClientToServer(const char *list_name, const std::string &fl_id) {
  bool retcode = true;
  ps::FLId fl_id_pb;
  fl_id_pb.set_fl_id(fl_id);
  ps::PBMetadata client_pb;
  client_pb.mutable_fl_id()->MergeFrom(fl_id_pb);
  retcode = ps::server::DistributedMetadataStore::GetInstance().UpdateMetadata(list_name, client_pb);
  return retcode;
}
void CipherMetaStorage::RegisterPrime(const char *list_name, const std::string &prime) {
  ps::Prime prime_id_pb;
  prime_id_pb.set_prime(prime);
  ps::PBMetadata prime_pb;
  prime_pb.mutable_prime()->MergeFrom(prime_id_pb);
  ps::server::DistributedMetadataStore::GetInstance().RegisterMetadata(list_name, prime_pb);
}

bool CipherMetaStorage::UpdateClientKeyToServer(const char *list_name, const std::string &fl_id,
                                                const std::vector<std::vector<unsigned char>> &cur_public_key) {
  bool retcode = true;
  if (cur_public_key.size() < 2) {
    MS_LOG(ERROR) << "cur_public_key's size must is 2. actual size is " << cur_public_key.size();
    return false;
  }
  // update new item to memory server.
  ps::KeysPb keys;
  keys.add_key()->assign(cur_public_key[0].begin(), cur_public_key[0].end());
  keys.add_key()->assign(cur_public_key[1].begin(), cur_public_key[1].end());
  ps::PairClientKeys pair_client_keys_pb;
  pair_client_keys_pb.set_fl_id(fl_id);
  pair_client_keys_pb.mutable_client_keys()->MergeFrom(keys);
  ps::PBMetadata client_and_keys_pb;
  client_and_keys_pb.mutable_pair_client_keys()->MergeFrom(pair_client_keys_pb);
  retcode = ps::server::DistributedMetadataStore::GetInstance().UpdateMetadata(list_name, client_and_keys_pb);
  return retcode;
}

bool CipherMetaStorage::UpdateClientNoiseToServer(const char *list_name, const std::vector<float> &cur_public_noise) {
  // update new item to memory server.
  ps::OneClientNoises noises_pb;
  *noises_pb.mutable_noise() = {cur_public_noise.begin(), cur_public_noise.end()};
  ps::PBMetadata client_noises_pb;
  client_noises_pb.mutable_one_client_noises()->MergeFrom(noises_pb);
  return ps::server::DistributedMetadataStore::GetInstance().UpdateMetadata(list_name, client_noises_pb);
}

bool CipherMetaStorage::UpdateClientShareToServer(
  const char *list_name, const std::string &fl_id,
  const flatbuffers::Vector<flatbuffers::Offset<mindspore::schema::ClientShare>> *shares) {
  bool retcode = true;
  int size_shares = shares->size();
  ps::SharesPb shares_pb;
  for (int index = 0; index < size_shares; ++index) {
    // new item
    ps::ClientShareStr *client_share_str_new_p = shares_pb.add_clientsharestrs();
    std::string fl_id_new = (*shares)[index]->fl_id()->str();
    int index_new = (*shares)[index]->index();
    auto share = (*shares)[index]->share();
    client_share_str_new_p->set_share(reinterpret_cast<const char *>(share->data()), share->size());
    client_share_str_new_p->set_fl_id(fl_id_new);
    client_share_str_new_p->set_index(index_new);
  }
  ps::PairClientShares pair_client_shares_pb;
  pair_client_shares_pb.set_fl_id(fl_id);
  pair_client_shares_pb.mutable_client_shares()->MergeFrom(shares_pb);
  ps::PBMetadata client_and_shares_pb;
  client_and_shares_pb.mutable_pair_client_shares()->MergeFrom(pair_client_shares_pb);
  retcode = ps::server::DistributedMetadataStore::GetInstance().UpdateMetadata(list_name, client_and_shares_pb);
  return retcode;
}

void CipherMetaStorage::RegisterClass() {
  ps::PBMetadata exchange_kyes_client_list;
  ps::server::DistributedMetadataStore::GetInstance().RegisterMetadata(ps::server::kCtxExChangeKeysClientList,
                                                                       exchange_kyes_client_list);
  ps::PBMetadata clients_keys;
  ps::server::DistributedMetadataStore::GetInstance().RegisterMetadata(ps::server::kCtxClientsKeys, clients_keys);
  ps::PBMetadata reconstruct_client_list;
  ps::server::DistributedMetadataStore::GetInstance().RegisterMetadata(ps::server::kCtxReconstructClientList,
                                                                       reconstruct_client_list);
  ps::PBMetadata clients_reconstruct_shares;
  ps::server::DistributedMetadataStore::GetInstance().RegisterMetadata(ps::server::kCtxClientsReconstructShares,
                                                                       clients_reconstruct_shares);
  ps::PBMetadata share_secretes_client_list;
  ps::server::DistributedMetadataStore::GetInstance().RegisterMetadata(ps::server::kCtxShareSecretsClientList,
                                                                       share_secretes_client_list);
  ps::PBMetadata clients_encrypt_shares;
  ps::server::DistributedMetadataStore::GetInstance().RegisterMetadata(ps::server::kCtxClientsEncryptedShares,
                                                                       clients_encrypt_shares);
}
}  // namespace armour
}  // namespace mindspore
