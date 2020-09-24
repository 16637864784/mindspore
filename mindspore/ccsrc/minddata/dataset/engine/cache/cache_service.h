/**
 * Copyright 2020 Huawei Technologies Co., Ltd

 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at

 * http://www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/

#ifndef MINDSPORE_CCSRC_MINDDATA_DATASET_ENGINE_CACHE_SERVICE_H_
#define MINDSPORE_CCSRC_MINDDATA_DATASET_ENGINE_CACHE_SERVICE_H_

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "minddata/dataset/core/global_context.h"
#include "minddata/dataset/core/tensor.h"
#include "minddata/dataset/engine/cache/cache_request.h"
#include "minddata/dataset/util/arena.h"
#include "minddata/dataset/util/btree.h"
#include "minddata/dataset/util/cache_pool.h"
#include "minddata/dataset/util/service.h"
#include "minddata/dataset/util/services.h"
#include "minddata/dataset/util/system_pool.h"

namespace mindspore {
namespace dataset {
/// Some typedef used for BatchFetch
using key_size_pair = std::pair<CachePool::key_type, size_t>;
/// \brief A cache service for storing/fetching buffers to in memory cache and may spill to disk the cache service is
/// created to support spilling
class CacheService : public Service {
 public:
  friend class CacheServer;

  enum class State : uint8_t { kNone = 0, kBuildPhase, kFetchPhase, kNoLocking };

  /// \brief Constructor
  /// \param mem_sz Memory size to be set aside for the in memory cache. 0 means unlimited
  /// \param root Spill path. Empty string means no spilling
  /// \param generate_id If the cache service should generate row id for buffer that is cached.
  /// For non-mappable dataset, this should be set to true.
  CacheService(uint64_t mem_sz, const std::string &root, bool generate_id);
  ~CacheService();

  /// \brief For fixed size memory, we will create an Arena.
  /// \return false if unlimited memory.
  bool UseArena();

  Status DoServiceStart() override;
  Status DoServiceStop() override;

  /// \brief Main function to cache a row which is in form a series of buffers.
  /// The first buffer is a Google flatbuffer which describes the rest of the buffers followed.
  /// \param[in] buf Vector of buffer
  /// \param[out] row_id_generated The row id assigned to this row if any
  /// \return Status object
  Status CacheRow(const std::vector<const void *> &buf, row_id_type *row_id_generated);

  /// \brief A fast version of CacheRow where all the data is already in one contiguous piece.
  /// \param src Slice of the data
  /// \param row_id_generated
  /// \return Status object
  Status FastCacheRow(const ReadableSlice &src, row_id_type *row_id_generated);

  /// \brief This function is used in preparation for batch fetching.
  /// It calculates how much memory we should allocate and which row id are present.
  /// \param[in/out] Pointer to vector of <CachePool::key_type, size_t>
  /// \param[in/out] mem_sz how much memory is required to batch fetch
  /// \return Status object
  Status PreBatchFetch(const std::vector<row_id_type> &v, std::vector<key_size_pair> *, int64_t *mem_sz);

  /// \brief Main function to fetch rows in batch. The output is a contiguous memory which will be decoded
  /// by the CacheClient. Cache miss is not an error, and will be coded in the output to mark an empty row.
  /// \param[in] v A vector of row id.
  /// \param[out] out A contiguous memory buffer that holds the requested rows.
  /// \return Status object
  Status BatchFetch(const std::vector<row_id_type> &v, const std::vector<key_size_pair> &, WritableSlice *out) const;

  /// \brief Getter function
  /// \return Spilling path
  Path GetSpillPath() const;
  /// \brief A structure returned from the cache server for statistics request.
  class ServiceStat {
   public:
    using state_type = std::underlying_type<State>::type;
    ServiceStat() : state_(0) {}
    ~ServiceStat() = default;
    CachePool::CacheStat stat_{};
    state_type state_;
  };
  /// \brief Statistics for the current service
  /// \param[in/out] A pointer to a pre-allocated ServiceStat structure
  /// \return Status Object
  Status GetStat(ServiceStat *);
  /// \brief Cache schema
  /// \param buf A Google Flatbuffer that contains the schema
  /// \param len size of the buffer
  /// \return Status object
  Status CacheSchema(const void *buf, int64_t len);
  /// \brief Fetch schema
  /// \param out A contiguous memory that contains the serialized form of schema.
  /// \return Status object
  Status FetchSchema(std::string *out) const;
  /// \brief Return a set of keys that are definitely cache miss
  /// \return Status object
  Status FindKeysMiss(std::vector<row_id_type> *out);
  /// \brief Overload the << operator to print a cache service
  /// \param out std::ostream
  /// \param cs A cache service
  /// \return std::ostream
  friend std::ostream &operator<<(std::ostream &out, const CacheService &cs);
  /// \brief Every cache service has a cookie. If the cookie of a CacheClient matches this cookie, this CacheClient
  /// is the creator
  /// \return Cookie
  std::string cookie() const { return cookie_; }
  /// \brief If this cache service generates row id for buffer cached, it is divided into two phases, a build phase and
  /// a read phase.
  /// \return True if has two phases.
  bool HasBuildPhase() const { return generate_id_; }
  /// \brief Change from write phase to read phase. Only the creator of this service is allowed to make this call.
  /// \return Status object
  Status BuildPhaseDone();
  /// \brief Find out the current memory usage
  int64_t GetMemoryUsage() { return cur_mem_usage_; }
  /// \brief Find out the current disk usage
  int64_t GetDiskUsage() { return cur_disk_usage_; }
  /// \brief For kToggleWriteMode request
  Status ToggleWriteMode(bool on_off);

 private:
  mutable RWLock rw_lock_;
  std::string root_;
  uint64_t cache_mem_sz_;
  std::shared_ptr<CachePool> cp_;
  std::atomic<row_id_type> next_id_;
  bool generate_id_;
  std::string cookie_;
  State st_;
  std::string schema_;
  // If we use an Arena, cur_disk_usage is always 0 as we don't know how CachePool manages it.
  // Otherwise we track how much is in memory and how much is on disk (if root_ is not empty).
  // We use them to control when we should stop caching in memory in the case when there is no
  // Arena.
  std::atomic<int64_t> cur_mem_usage_;
  std::atomic<int64_t> cur_disk_usage_;
  // We also cache the result from calling FindKeysMiss because it is expensive. Besides user make
  // this request after we hit memory full or disk full. So the result is unlikely to change.
  std::mutex get_key_miss_mux_;
  std::shared_ptr<std::vector<row_id_type>> key_miss_results_;
  /// \brief Private function to generate a row id
  /// \return Row id assigned.
  row_id_type GetNextRowId() { return next_id_.fetch_add(1); }
};
}  // namespace dataset
}  // namespace mindspore
#endif  // MINDSPORE_CCSRC_MINDDATA_DATASET_ENGINE_CACHE_SERVICE_H_
