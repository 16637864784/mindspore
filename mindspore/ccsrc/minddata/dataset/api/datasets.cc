/**
 * Copyright 2020 Huawei Technologies Co., Ltd
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

#include <fstream>
#include <unordered_set>
#include <algorithm>
#include "minddata/dataset/include/datasets.h"
#include "minddata/dataset/include/samplers.h"
#include "minddata/dataset/include/transforms.h"
#include "minddata/dataset/engine/dataset_iterator.h"
// Source dataset headers (in alphabetical order)
#include "minddata/dataset/engine/datasetops/source/album_op.h"
#include "minddata/dataset/engine/datasetops/source/celeba_op.h"
#include "minddata/dataset/engine/datasetops/source/cifar_op.h"
#include "minddata/dataset/engine/datasetops/source/clue_op.h"
#include "minddata/dataset/engine/datasetops/source/coco_op.h"
#include "minddata/dataset/engine/datasetops/source/csv_op.h"
#include "minddata/dataset/engine/datasetops/source/image_folder_op.h"
#ifndef ENABLE_ANDROID
#include "minddata/dataset/engine/datasetops/source/manifest_op.h"
#endif
#include "minddata/dataset/engine/datasetops/source/mnist_op.h"
#include "minddata/dataset/engine/datasetops/source/random_data_op.h"
#include "minddata/dataset/engine/datasetops/source/text_file_op.h"
#ifndef ENABLE_ANDROID
#include "minddata/dataset/engine/datasetops/source/tf_reader_op.h"
#include "minddata/dataset/engine/datasetops/source/voc_op.h"
#endif
// Dataset operator headers (in alphabetical order)
#include "minddata/dataset/engine/datasetops/batch_op.h"
#ifndef ENABLE_ANDROID
#include "minddata/dataset/engine/datasetops/bucket_batch_by_length_op.h"
#endif
#include "minddata/dataset/engine/datasetops/build_vocab_op.h"
#include "minddata/dataset/engine/datasetops/concat_op.h"
#include "minddata/dataset/engine/datasetops/map_op/map_op.h"
#include "minddata/dataset/engine/datasetops/project_op.h"
#include "minddata/dataset/engine/datasetops/rename_op.h"
#include "minddata/dataset/engine/datasetops/repeat_op.h"
#include "minddata/dataset/engine/datasetops/shuffle_op.h"
#include "minddata/dataset/engine/datasetops/skip_op.h"
#include "minddata/dataset/engine/datasetops/take_op.h"
#include "minddata/dataset/engine/datasetops/zip_op.h"

// Sampler headers (in alphabetical order)
#include "minddata/dataset/engine/datasetops/source/sampler/sampler.h"
#include "minddata/dataset/engine/datasetops/source/sampler/random_sampler.h"
#include "minddata/dataset/engine/datasetops/source/sampler/sequential_sampler.h"

#include "minddata/dataset/core/config_manager.h"
#include "minddata/dataset/util/random.h"
#include "minddata/dataset/util/path.h"

namespace mindspore {
namespace dataset {
namespace api {

#define RETURN_EMPTY_IF_ERROR(_s) \
  do {                            \
    Status __rc = (_s);           \
    if (__rc.IsError()) {         \
      MS_LOG(ERROR) << __rc;      \
      return {};                  \
    }                             \
  } while (false)

// Function to create the iterator, which will build and launch the execution tree.
std::shared_ptr<Iterator> Dataset::CreateIterator(std::vector<std::string> columns) {
  std::shared_ptr<Iterator> iter;
  try {
    auto ds = shared_from_this();

    // The specified columns will be selected from the dataset and passed down the pipeline
    // in the order specified, other columns will be discarded.
    if (!columns.empty()) {
      ds = ds->Project(columns);
    }

    iter = std::make_shared<Iterator>();
    Status rc = iter->BuildAndLaunchTree(ds);
    if (rc.IsError()) {
      MS_LOG(ERROR) << "CreateIterator failed." << rc;
      return nullptr;
    }

    return iter;
  } catch (const std::exception &err) {
    MS_LOG(ERROR) << "CreateIterator: Iterator exception caught: " << err.what();
    return nullptr;
  }

  return iter;
}

// Constructor
Dataset::Dataset() {
  // Fetch some default value from config manager
  std::shared_ptr<ConfigManager> cfg = GlobalContext::config_manager();
  num_workers_ = cfg->num_parallel_workers();
  rows_per_buffer_ = cfg->rows_per_buffer();
  connector_que_size_ = cfg->op_connector_size();
  worker_connector_size_ = cfg->worker_connector_size();
}

/// \brief Function to create a SchemaObj
/// \param[in] schema_file Path of schema file
/// \return Shared pointer to the current schema
std::shared_ptr<SchemaObj> Schema(const std::string &schema_file) {
  auto schema = std::make_shared<SchemaObj>(schema_file);

  return schema->init() ? schema : nullptr;
}

// FUNCTIONS TO CREATE DATASETS FOR LEAF-NODE DATASETS
// (In alphabetical order)

// Function to create a AlbumDataset.
std::shared_ptr<AlbumDataset> Album(const std::string &dataset_dir, const std::string &data_schema,
                                    const std::vector<std::string> &column_names, bool decode,
                                    const std::shared_ptr<SamplerObj> &sampler) {
  auto ds = std::make_shared<AlbumDataset>(dataset_dir, data_schema, column_names, decode, sampler);

  return ds->ValidateParams() ? ds : nullptr;
}

// Function to create a CelebADataset.
std::shared_ptr<CelebADataset> CelebA(const std::string &dataset_dir, const std::string &usage,
                                      const std::shared_ptr<SamplerObj> &sampler, bool decode,
                                      const std::set<std::string> &extensions) {
  auto ds = std::make_shared<CelebADataset>(dataset_dir, usage, sampler, decode, extensions);

  // Call derived class validation method.
  return ds->ValidateParams() ? ds : nullptr;
}

// Function to create a Cifar10Dataset.
std::shared_ptr<Cifar10Dataset> Cifar10(const std::string &dataset_dir, const std::string &usage,
                                        const std::shared_ptr<SamplerObj> &sampler) {
  auto ds = std::make_shared<Cifar10Dataset>(dataset_dir, usage, sampler);

  // Call derived class validation method.
  return ds->ValidateParams() ? ds : nullptr;
}

// Function to create a Cifar100Dataset.
std::shared_ptr<Cifar100Dataset> Cifar100(const std::string &dataset_dir, const std::string &usage,
                                          const std::shared_ptr<SamplerObj> &sampler) {
  auto ds = std::make_shared<Cifar100Dataset>(dataset_dir, usage, sampler);

  // Call derived class validation method.
  return ds->ValidateParams() ? ds : nullptr;
}

// Function to create a CLUEDataset.
std::shared_ptr<CLUEDataset> CLUE(const std::vector<std::string> &clue_files, const std::string &task,
                                  const std::string &usage, int64_t num_samples, ShuffleMode shuffle,
                                  int32_t num_shards, int32_t shard_id) {
  auto ds = std::make_shared<CLUEDataset>(clue_files, task, usage, num_samples, shuffle, num_shards, shard_id);

  // Call derived class validation method.
  return ds->ValidateParams() ? ds : nullptr;
}

// Function to create a CocoDataset.
std::shared_ptr<CocoDataset> Coco(const std::string &dataset_dir, const std::string &annotation_file,
                                  const std::string &task, const bool &decode,
                                  const std::shared_ptr<SamplerObj> &sampler) {
  auto ds = std::make_shared<CocoDataset>(dataset_dir, annotation_file, task, decode, sampler);

  // Call derived class validation method.
  return ds->ValidateParams() ? ds : nullptr;
}

// Function to create a CSVDataset.
std::shared_ptr<CSVDataset> CSV(const std::vector<std::string> &dataset_files, char field_delim,
                                const std::vector<std::shared_ptr<CsvBase>> &column_defaults,
                                const std::vector<std::string> &column_names, int64_t num_samples, ShuffleMode shuffle,
                                int32_t num_shards, int32_t shard_id) {
  auto ds = std::make_shared<CSVDataset>(dataset_files, field_delim, column_defaults, column_names, num_samples,
                                         shuffle, num_shards, shard_id);

  // Call derived class validation method.
  return ds->ValidateParams() ? ds : nullptr;
}

// Function to create a ImageFolderDataset.
std::shared_ptr<ImageFolderDataset> ImageFolder(const std::string &dataset_dir, bool decode,
                                                const std::shared_ptr<SamplerObj> &sampler,
                                                const std::set<std::string> &extensions,
                                                const std::map<std::string, int32_t> &class_indexing) {
  // This arg exists in ImageFolderOp, but not externalized (in Python API). The default value is false.
  bool recursive = false;

  // Create logical representation of ImageFolderDataset.
  auto ds = std::make_shared<ImageFolderDataset>(dataset_dir, decode, sampler, recursive, extensions, class_indexing);

  // Call derived class validation method.
  return ds->ValidateParams() ? ds : nullptr;
}

#ifndef ENABLE_ANDROID
// Function to create a ManifestDataset.
std::shared_ptr<ManifestDataset> Manifest(const std::string &dataset_file, const std::string &usage,
                                          const std::shared_ptr<SamplerObj> &sampler,
                                          const std::map<std::string, int32_t> &class_indexing, bool decode) {
  auto ds = std::make_shared<ManifestDataset>(dataset_file, usage, sampler, class_indexing, decode);

  // Call derived class validation method.
  return ds->ValidateParams() ? ds : nullptr;
}
#endif

// Function to create a MnistDataset.
std::shared_ptr<MnistDataset> Mnist(const std::string &dataset_dir, const std::string &usage,
                                    const std::shared_ptr<SamplerObj> &sampler) {
  auto ds = std::make_shared<MnistDataset>(dataset_dir, usage, sampler);

  // Call derived class validation method.
  return ds->ValidateParams() ? ds : nullptr;
}

// Function to overload "+" operator to concat two datasets
std::shared_ptr<ConcatDataset> operator+(const std::shared_ptr<Dataset> &datasets1,
                                         const std::shared_ptr<Dataset> &datasets2) {
  std::shared_ptr<ConcatDataset> ds = std::make_shared<ConcatDataset>(std::vector({datasets2, datasets1}));

  // Call derived class validation method.
  return ds->ValidateParams() ? ds : nullptr;
}

// Function to create a TextFileDataset.
std::shared_ptr<TextFileDataset> TextFile(const std::vector<std::string> &dataset_files, int64_t num_samples,
                                          ShuffleMode shuffle, int32_t num_shards, int32_t shard_id) {
  auto ds = std::make_shared<TextFileDataset>(dataset_files, num_samples, shuffle, num_shards, shard_id);

  // Call derived class validation method.
  return ds->ValidateParams() ? ds : nullptr;
}

#ifndef ENABLE_ANDROID
// Function to create a VOCDataset.
std::shared_ptr<VOCDataset> VOC(const std::string &dataset_dir, const std::string &task, const std::string &usage,
                                const std::map<std::string, int32_t> &class_indexing, bool decode,
                                const std::shared_ptr<SamplerObj> &sampler) {
  auto ds = std::make_shared<VOCDataset>(dataset_dir, task, usage, class_indexing, decode, sampler);

  // Call derived class validation method.
  return ds->ValidateParams() ? ds : nullptr;
}
#endif

// Function to create a ZipDataset.
std::shared_ptr<ZipDataset> Zip(const std::vector<std::shared_ptr<Dataset>> &datasets) {
  auto ds = std::make_shared<ZipDataset>(datasets);

  // Call derived class validation method.
  return ds->ValidateParams() ? ds : nullptr;
}

// FUNCTIONS TO CREATE DATASETS FOR DATASET OPS
// (In alphabetical order)

// Function to create a Batch dataset
std::shared_ptr<BatchDataset> Dataset::Batch(int32_t batch_size, bool drop_remainder) {
  // Default values
  std::vector<std::string> cols_to_map = {};
  std::map<std::string, std::pair<TensorShape, std::shared_ptr<Tensor>>> pad_map;
  bool pad = false;
  auto ds = std::make_shared<BatchDataset>(batch_size, drop_remainder, pad, cols_to_map, pad_map);

  if (!ds->ValidateParams()) {
    return nullptr;
  }

  ds->children.push_back(shared_from_this());

  return ds;
}

#ifndef ENABLE_ANDROID
// Function to create a BucketBatchByLength dataset
std::shared_ptr<BucketBatchByLengthDataset> Dataset::BucketBatchByLength(
  const std::vector<std::string> &column_names, const std::vector<int32_t> &bucket_boundaries,
  const std::vector<int32_t> &bucket_batch_sizes, TensorRow (*element_length_function)(TensorRow),
  const std::map<std::string, std::pair<TensorShape, std::shared_ptr<Tensor>>> &pad_info, bool pad_to_bucket_boundary,
  bool drop_remainder) {
  auto ds = std::make_shared<BucketBatchByLengthDataset>(column_names, bucket_boundaries, bucket_batch_sizes,
                                                         element_length_function, pad_info, pad_to_bucket_boundary,
                                                         drop_remainder);

  if (!ds->ValidateParams()) {
    return nullptr;
  }

  ds->children.push_back(shared_from_this());

  return ds;
}

// Function to create a Vocab from dataset
std::shared_ptr<Vocab> Dataset::BuildVocab(const std::vector<std::string> &columns,
                                           const std::pair<int64_t, int64_t> &freq_range, int64_t top_k,
                                           const std::vector<std::string> &special_tokens, bool special_first) {
  auto vocab = std::make_shared<Vocab>();
  auto ds = std::make_shared<BuildVocabDataset>(vocab, columns, freq_range, top_k, special_tokens, special_first);

  if (!ds->ValidateParams()) {
    return nullptr;
  }

  ds->children.push_back(shared_from_this());

  // Run tree here to starting building vocab
  std::shared_ptr<Iterator> iter = ds->CreateIterator();
  if (iter == nullptr) {
    MS_LOG(ERROR) << "Fail to run iterator in BuildVocab.";
    return nullptr;
  }

  // Finish building vocab by triggering GetNextRow
  std::unordered_map<std::string, std::shared_ptr<Tensor>> row;
  if (!iter->GetNextRow(&row)) {
    return nullptr;
  }

  return vocab;
}
#endif

// Function to create a Concat dataset
std::shared_ptr<ConcatDataset> Dataset::Concat(const std::vector<std::shared_ptr<Dataset>> &datasets) {
  auto ds = std::make_shared<ConcatDataset>(datasets);
  ds->children.push_back(shared_from_this());

  return ds->ValidateParams() ? ds : nullptr;
}

// Function to create a Map dataset.
std::shared_ptr<MapDataset> Dataset::Map(std::vector<std::shared_ptr<TensorOperation>> operations,
                                         std::vector<std::string> input_columns,
                                         std::vector<std::string> output_columns,
                                         const std::vector<std::string> &project_columns) {
  auto ds = std::make_shared<MapDataset>(operations, input_columns, output_columns, project_columns);

  if (!ds->ValidateParams()) {
    return nullptr;
  }

  ds->children.push_back(shared_from_this());

  return ds;
}

// Function to create a ProjectDataset.
std::shared_ptr<ProjectDataset> Dataset::Project(const std::vector<std::string> &columns) {
  auto ds = std::make_shared<ProjectDataset>(columns);
  // Call derived class validation method.
  if (!ds->ValidateParams()) {
    return nullptr;
  }

  ds->children.push_back(shared_from_this());

  return ds;
}

// Function to create a RenameDataset.
std::shared_ptr<RenameDataset> Dataset::Rename(const std::vector<std::string> &input_columns,
                                               const std::vector<std::string> &output_columns) {
  auto ds = std::make_shared<RenameDataset>(input_columns, output_columns);
  // Call derived class validation method.
  if (!ds->ValidateParams()) {
    return nullptr;
  }

  ds->children.push_back(shared_from_this());

  return ds;
}

// Function to create Repeat dataset.
std::shared_ptr<Dataset> Dataset::Repeat(int32_t count) {
  // Workaround for repeat == 1, do not inject repeat.
  if (count == 1) {
    return shared_from_this();
  }

  auto ds = std::make_shared<RepeatDataset>(count);

  if (!ds->ValidateParams()) {
    return nullptr;
  }

  ds->children.push_back(shared_from_this());

  return ds;
}

// Function to create a ShuffleOp
std::shared_ptr<ShuffleDataset> Dataset::Shuffle(int32_t buffer_size) {
  // Pass in reshuffle_each_epoch with true
  auto ds = std::make_shared<ShuffleDataset>(buffer_size, true);

  if (!ds->ValidateParams()) {
    return nullptr;
  }

  ds->children.push_back(shared_from_this());

  return ds;
}

// Function to create a SkipDataset.
std::shared_ptr<SkipDataset> Dataset::Skip(int32_t count) {
  auto ds = std::make_shared<SkipDataset>(count);

  // Call derived class validation method.
  if (!ds->ValidateParams()) {
    return nullptr;
  }

  ds->children.push_back(shared_from_this());

  return ds;
}

// Function to create a TakeDataset.
std::shared_ptr<Dataset> Dataset::Take(int32_t count) {
  // If count is greater than the number of element in dataset or equal to -1,
  // all the element in dataset will be taken
  if (count == -1) {
    return shared_from_this();
  }

  auto ds = std::make_shared<TakeDataset>(count);

  // Call derived class validation method.
  if (!ds->ValidateParams()) {
    return nullptr;
  }

  ds->children.push_back(shared_from_this());

  return ds;
}

// Function to create a Zip dataset
std::shared_ptr<ZipDataset> Dataset::Zip(const std::vector<std::shared_ptr<Dataset>> &datasets) {
  // Default values
  auto ds = std::make_shared<ZipDataset>(datasets);
  ds->children.push_back(shared_from_this());

  return ds->ValidateParams() ? ds : nullptr;
}

SchemaObj::SchemaObj(const std::string &schema_file) : schema_file_(schema_file), num_rows_(0), dataset_type_("") {}

// SchemaObj init function
bool SchemaObj::init() {
  if (schema_file_ != "") {
    Path schema_file(schema_file_);
    if (!schema_file.Exists()) {
      MS_LOG(ERROR) << "The file " << schema_file << " does not exist or permission denied!";
      return false;
    }

    nlohmann::json js;
    try {
      std::ifstream in(schema_file_);
      in >> js;
      if (js.find("columns") == js.end()) {
        MS_LOG(ERROR) << "\"columns\" node is required in the schema json file.";
        return false;
      }
    } catch (const std::exception &err) {
      MS_LOG(ERROR) << "Schema file failed to load";
      return false;
    }
    return from_json(js);
  }
  return true;
}

// Function to add a column to schema with a mstype de_type
bool SchemaObj::add_column(std::string name, TypeId de_type, std::vector<int32_t> shape) {
  nlohmann::json new_column;
  new_column["name"] = name;
  // if de_type is mstype
  DataType data_type = dataset::MSTypeToDEType(de_type);
  new_column["type"] = data_type.ToString();
  if (shape.size() > 0) {
    new_column["shape"] = shape;
    new_column["rank"] = shape.size();
  } else {
    new_column["rank"] = 1;
  }
  columns_.push_back(new_column);
  return true;
}

// Function to add a column to schema with a string de_type
bool SchemaObj::add_column(std::string name, std::string de_type, std::vector<int32_t> shape) {
  nlohmann::json new_column;
  new_column["name"] = name;
  DataType data_type(de_type);
  new_column["type"] = data_type.ToString();
  if (shape.size() > 0) {
    new_column["shape"] = shape;
    new_column["rank"] = shape.size();
  } else {
    new_column["rank"] = 1;
  }
  columns_.push_back(new_column);
  return true;
}

std::string SchemaObj::to_json() {
  nlohmann::json json_file;
  json_file["columns"] = columns_;
  if (dataset_type_ != "") {
    json_file["datasetType"] = dataset_type_;
  }

  if (num_rows_ > 0) {
    json_file["numRows"] = num_rows_;
  }

  return json_file.dump(2);
}

bool SchemaObj::parse_column(nlohmann::json columns) {
  std::string name, de_type;
  std::vector<int32_t> shape;

  columns_.clear();
  if (columns.type() == nlohmann::json::value_t::array) {
    // reference to python list
    for (auto column : columns) {
      auto key_name = column.find("name");
      if (key_name == column.end()) {
        MS_LOG(ERROR) << "Column's name is missing";
        return false;
      }
      name = *key_name;

      auto key_type = column.find("type");
      if (key_type == column.end()) {
        MS_LOG(ERROR) << "Column's type is missing";
        return false;
      }
      de_type = *key_type;

      shape.clear();
      auto key_shape = column.find("shape");
      if (key_shape != column.end()) {
        shape.insert(shape.end(), (*key_shape).begin(), (*key_shape).end());
      }
      if (!add_column(name, de_type, shape)) {
        return false;
      }
    }
  } else if (columns.type() == nlohmann::json::value_t::object) {
    for (const auto &it_child : columns.items()) {
      name = it_child.key();
      auto key_type = it_child.value().find("type");
      if (key_type == it_child.value().end()) {
        MS_LOG(ERROR) << "Column's type is missing";
        return false;
      }
      de_type = *key_type;

      shape.clear();
      auto key_shape = it_child.value().find("shape");
      if (key_shape != it_child.value().end()) {
        shape.insert(shape.end(), (*key_shape).begin(), (*key_shape).end());
      }

      if (!add_column(name, de_type, shape)) {
        return false;
      }
    }
  } else {
    MS_LOG(ERROR) << "columns must be dict or list, columns contain name, type, shape(optional).";
    return false;
  }
  return true;
}

bool SchemaObj::from_json(nlohmann::json json_obj) {
  for (const auto &it_child : json_obj.items()) {
    if (it_child.key() == "datasetType") {
      dataset_type_ = it_child.value();
    } else if (it_child.key() == "numRows") {
      num_rows_ = it_child.value();
    } else if (it_child.key() == "columns") {
      if (!parse_column(it_child.value())) {
        MS_LOG(ERROR) << "parse columns failed";
        return false;
      }
    } else {
      MS_LOG(ERROR) << "Unknown field " << it_child.key();
      return false;
    }
  }
  if (columns_.empty()) {
    MS_LOG(ERROR) << "Columns are missing.";
    return false;
  }
  if (num_rows_ <= 0) {
    MS_LOG(ERROR) << "numRows must be greater than 0";
    return false;
  }

  return true;
}

// OTHER FUNCTIONS

// Helper function to compute a default shuffle size
Status ComputeShuffleSize(int64_t num_files, int64_t num_devices, int64_t num_rows, int64_t total_rows,
                          int64_t *shuffle_size) {
  const int64_t average_files_multiplier = 4;
  const int64_t shuffle_max = 10000;
  int64_t avg_rows_per_file = 0;

  // Adjust the num rows per shard if sharding was given
  if (num_devices > 0) {
    if (num_rows % num_devices == 0) {
      num_rows = num_rows / num_devices;
    } else {
      num_rows = (num_rows / num_devices) + 1;
    }
  }

  // Cap based on total rows directive.  Some ops do not have this and give value of 0.
  if (total_rows > 0) {
    num_rows = std::min(num_rows, total_rows);
  }

  // get the average per file
  CHECK_FAIL_RETURN_UNEXPECTED(num_files != 0, "The size of dataset_files must greater than 0.");
  avg_rows_per_file = num_rows / num_files;

  *shuffle_size = std::max(avg_rows_per_file * average_files_multiplier, shuffle_max);
  return Status::OK();
}

// Helper function to inject a shuffle operator over top of current operator being built
Status AddShuffleOp(int64_t num_files, int64_t num_devices, int64_t num_rows, int64_t total_rows,
                    int32_t connector_que_size, int32_t rows_per_buffer, std::shared_ptr<DatasetOp> *shuffle_op) {
  std::shared_ptr<ShuffleOp> new_shuffle_op = nullptr;
  int64_t shuffle_size = 0;
  RETURN_EMPTY_IF_ERROR(ComputeShuffleSize(num_files, num_devices, num_rows, total_rows, &shuffle_size));
  MS_LOG(INFO) << "Dataset::AddShuffleOp - num_rows: " << num_rows << ", shuffle_size: " << shuffle_size;
  // Add the shuffle op
  *shuffle_op = std::make_shared<ShuffleOp>(shuffle_size, GetSeed(), connector_que_size, true, rows_per_buffer);
  return Status::OK();
}

// Helper function to validate dataset directory parameter
bool ValidateDatasetDirParam(const std::string &dataset_name, std::string dataset_dir) {
  if (dataset_dir.empty()) {
    MS_LOG(ERROR) << dataset_name << ": dataset_dir is not specified.";
    return false;
  }

  Path dir(dataset_dir);
  if (!dir.IsDirectory()) {
    MS_LOG(ERROR) << dataset_name << ": dataset_dir: [" << dataset_dir << "] is an invalid directory path.";
    return false;
  }

  if (access(dataset_dir.c_str(), R_OK) == -1) {
    MS_LOG(ERROR) << dataset_name << ": No access to specified dataset path: " << dataset_dir;
    return false;
  }

  return true;
}

// Helper function to validate dataset dataset files parameter
bool ValidateDatasetFilesParam(const std::string &dataset_name, const std::vector<std::string> &dataset_files) {
  if (dataset_files.empty()) {
    MS_LOG(ERROR) << dataset_name << ": dataset_files is not specified.";
    return false;
  }

  for (auto f : dataset_files) {
    Path dataset_file(f);
    if (!dataset_file.Exists()) {
      MS_LOG(ERROR) << dataset_name << ": dataset file: [" << f << "] is invalid or does not exist.";
      return false;
    }
  }

  return true;
}

// Helper function to validate dataset num_shards and shard_id parameters
bool ValidateDatasetShardParams(const std::string &dataset_name, int32_t num_shards, int32_t shard_id) {
  if (num_shards <= 0) {
    MS_LOG(ERROR) << dataset_name << ": Invalid num_shards: " << num_shards;
    return false;
  }

  if (shard_id < 0 || shard_id >= num_shards) {
    MS_LOG(ERROR) << dataset_name << ": Invalid input, shard_id: " << shard_id << ", num_shards: " << num_shards;
    return false;
  }

  return true;
}

// Helper function to validate dataset sampler parameter
bool ValidateDatasetSampler(const std::string &dataset_name, const std::shared_ptr<SamplerObj> &sampler) {
  if (sampler == nullptr) {
    MS_LOG(ERROR) << dataset_name << ": Sampler is not constructed correctly, sampler: nullptr";
    return false;
  }
  return true;
}

bool ValidateStringValue(const std::string &str, const std::unordered_set<std::string> &valid_strings) {
  if (valid_strings.find(str) == valid_strings.end()) {
    std::string mode;
    mode = std::accumulate(valid_strings.begin(), valid_strings.end(), mode,
                           [](std::string a, std::string b) { return std::move(a) + " " + std::move(b); });
    MS_LOG(ERROR) << str << " does not match any mode in [" + mode + " ]";
    return false;
  }
  return true;
}

// Helper function to validate dataset input/output column parameter
bool ValidateDatasetColumnParam(const std::string &dataset_name, const std::string &column_param,
                                const std::vector<std::string> &columns) {
  if (columns.empty()) {
    MS_LOG(ERROR) << dataset_name << ":" << column_param << " should not be empty string";
    return false;
  }
  for (uint32_t i = 0; i < columns.size(); ++i) {
    if (columns[i].empty()) {
      MS_LOG(ERROR) << dataset_name << ":" << column_param << "[" << i << "] should not be empty";
      return false;
    }
  }
  std::set<std::string> columns_set(columns.begin(), columns.end());
  if (columns_set.size() != columns.size()) {
    MS_LOG(ERROR) << dataset_name << ":" << column_param << ": Every column name should not be same with others";
    return false;
  }
  return true;
}

/* ####################################### Derived Dataset classes ################################# */

// DERIVED DATASET CLASSES LEAF-NODE DATASETS
// (In alphabetical order)

// Constructor for AlbumDataset
AlbumDataset::AlbumDataset(const std::string &dataset_dir, const std::string &data_schema,
                           const std::vector<std::string> &column_names, bool decode,
                           const std::shared_ptr<SamplerObj> &sampler)
    : dataset_dir_(dataset_dir),
      schema_path_(data_schema),
      column_names_(column_names),
      decode_(decode),
      sampler_(sampler) {}

bool AlbumDataset::ValidateParams() {
  if (!ValidateDatasetDirParam("AlbumDataset", dataset_dir_)) {
    return false;
  }

  if (!ValidateDatasetFilesParam("AlbumDataset", {schema_path_})) {
    return false;
  }

  if (!ValidateDatasetSampler("AlbumDataset", sampler_)) {
    return false;
  }

  if (!column_names_.empty()) {
    if (!ValidateDatasetColumnParam("AlbumDataset", "column_names", column_names_)) {
      return false;
    }
  }

  return true;
}

// Function to build AlbumDataset
std::vector<std::shared_ptr<DatasetOp>> AlbumDataset::Build() {
  // A vector containing shared pointer to the Dataset Ops that this object will create
  std::vector<std::shared_ptr<DatasetOp>> node_ops;

  auto schema = std::make_unique<DataSchema>();
  RETURN_EMPTY_IF_ERROR(schema->LoadSchemaFile(schema_path_, column_names_));

  // Argument that is not exposed to user in the API.
  std::set<std::string> extensions = {};

  node_ops.push_back(std::make_shared<AlbumOp>(num_workers_, rows_per_buffer_, dataset_dir_, connector_que_size_,
                                               decode_, extensions, std::move(schema), std::move(sampler_->Build())));
  return node_ops;
}

// Constructor for CelebADataset
CelebADataset::CelebADataset(const std::string &dataset_dir, const std::string &usage,
                             const std::shared_ptr<SamplerObj> &sampler, const bool &decode,
                             const std::set<std::string> &extensions)
    : dataset_dir_(dataset_dir), usage_(usage), sampler_(sampler), decode_(decode), extensions_(extensions) {}

bool CelebADataset::ValidateParams() {
  return ValidateDatasetDirParam("CelebADataset", dataset_dir_) && ValidateDatasetSampler("CelebADataset", sampler_) &&
         ValidateStringValue(usage_, {"all", "train", "valid", "test"});
}

// Function to build CelebADataset
std::vector<std::shared_ptr<DatasetOp>> CelebADataset::Build() {
  // A vector containing shared pointer to the Dataset Ops that this object will create
  std::vector<std::shared_ptr<DatasetOp>> node_ops;

  std::unique_ptr<DataSchema> schema = std::make_unique<DataSchema>();
  RETURN_EMPTY_IF_ERROR(
    schema->AddColumn(ColDescriptor("image", DataType(DataType::DE_UINT8), TensorImpl::kFlexible, 1)));
  // label is like this:0 1 0 0 1......
  RETURN_EMPTY_IF_ERROR(
    schema->AddColumn(ColDescriptor("attr", DataType(DataType::DE_UINT32), TensorImpl::kFlexible, 1)));
  node_ops.push_back(std::make_shared<CelebAOp>(num_workers_, rows_per_buffer_, dataset_dir_, connector_que_size_,
                                                decode_, usage_, extensions_, std::move(schema),
                                                std::move(sampler_->Build())));
  return node_ops;
}

// Constructor for Cifar10Dataset
Cifar10Dataset::Cifar10Dataset(const std::string &dataset_dir, const std::string &usage,
                               std::shared_ptr<SamplerObj> sampler)
    : dataset_dir_(dataset_dir), usage_(usage), sampler_(sampler) {}

bool Cifar10Dataset::ValidateParams() {
  return ValidateDatasetDirParam("Cifar10Dataset", dataset_dir_) &&
         ValidateDatasetSampler("Cifar10Dataset", sampler_) && ValidateStringValue(usage_, {"train", "test", "all"});
}

// Function to build CifarOp for Cifar10
std::vector<std::shared_ptr<DatasetOp>> Cifar10Dataset::Build() {
  // A vector containing shared pointer to the Dataset Ops that this object will create
  std::vector<std::shared_ptr<DatasetOp>> node_ops;

  // Do internal Schema generation.
  auto schema = std::make_unique<DataSchema>();
  RETURN_EMPTY_IF_ERROR(schema->AddColumn(ColDescriptor("image", DataType(DataType::DE_UINT8), TensorImpl::kCv, 1)));
  TensorShape scalar = TensorShape::CreateScalar();
  RETURN_EMPTY_IF_ERROR(
    schema->AddColumn(ColDescriptor("label", DataType(DataType::DE_UINT32), TensorImpl::kFlexible, 0, &scalar)));

  node_ops.push_back(std::make_shared<CifarOp>(CifarOp::CifarType::kCifar10, usage_, num_workers_, rows_per_buffer_,
                                               dataset_dir_, connector_que_size_, std::move(schema),
                                               std::move(sampler_->Build())));
  return node_ops;
}

// Constructor for Cifar100Dataset
Cifar100Dataset::Cifar100Dataset(const std::string &dataset_dir, const std::string &usage,
                                 std::shared_ptr<SamplerObj> sampler)
    : dataset_dir_(dataset_dir), usage_(usage), sampler_(sampler) {}

bool Cifar100Dataset::ValidateParams() {
  return ValidateDatasetDirParam("Cifar100Dataset", dataset_dir_) &&
         ValidateDatasetSampler("Cifar100Dataset", sampler_) && ValidateStringValue(usage_, {"train", "test", "all"});
}

// Function to build CifarOp for Cifar100
std::vector<std::shared_ptr<DatasetOp>> Cifar100Dataset::Build() {
  // A vector containing shared pointer to the Dataset Ops that this object will create
  std::vector<std::shared_ptr<DatasetOp>> node_ops;

  // Do internal Schema generation.
  auto schema = std::make_unique<DataSchema>();
  RETURN_EMPTY_IF_ERROR(schema->AddColumn(ColDescriptor("image", DataType(DataType::DE_UINT8), TensorImpl::kCv, 1)));
  TensorShape scalar = TensorShape::CreateScalar();
  RETURN_EMPTY_IF_ERROR(
    schema->AddColumn(ColDescriptor("coarse_label", DataType(DataType::DE_UINT32), TensorImpl::kFlexible, 0, &scalar)));
  RETURN_EMPTY_IF_ERROR(
    schema->AddColumn(ColDescriptor("fine_label", DataType(DataType::DE_UINT32), TensorImpl::kFlexible, 0, &scalar)));

  node_ops.push_back(std::make_shared<CifarOp>(CifarOp::CifarType::kCifar100, usage_, num_workers_, rows_per_buffer_,
                                               dataset_dir_, connector_que_size_, std::move(schema),
                                               std::move(sampler_->Build())));
  return node_ops;
}

// Constructor for CLUEDataset
CLUEDataset::CLUEDataset(const std::vector<std::string> clue_files, std::string task, std::string usage,
                         int64_t num_samples, ShuffleMode shuffle, int32_t num_shards, int32_t shard_id)
    : dataset_files_(clue_files),
      task_(task),
      usage_(usage),
      num_samples_(num_samples),
      shuffle_(shuffle),
      num_shards_(num_shards),
      shard_id_(shard_id) {}

bool CLUEDataset::ValidateParams() {
  if (!ValidateDatasetFilesParam("CLUEDataset", dataset_files_)) {
    return false;
  }

  std::vector<std::string> task_list = {"AFQMC", "TNEWS", "IFLYTEK", "CMNLI", "WSC", "CSL"};
  std::vector<std::string> usage_list = {"train", "test", "eval"};

  if (find(task_list.begin(), task_list.end(), task_) == task_list.end()) {
    MS_LOG(ERROR) << "task should be AFQMC, TNEWS, IFLYTEK, CMNLI, WSC or CSL.";
    return false;
  }

  if (find(usage_list.begin(), usage_list.end(), usage_) == usage_list.end()) {
    MS_LOG(ERROR) << "usage should be train, test or eval.";
    return false;
  }

  if (num_samples_ < 0) {
    MS_LOG(ERROR) << "CLUEDataset: Invalid number of samples: " << num_samples_;
    return false;
  }

  if (!ValidateDatasetShardParams("CLUEDataset", num_shards_, shard_id_)) {
    return false;
  }

  return true;
}

// Function to split string based on a character delimiter
std::vector<std::string> CLUEDataset::split(const std::string &s, char delim) {
  std::vector<std::string> res;
  std::stringstream ss(s);
  std::string item;

  while (getline(ss, item, delim)) {
    res.push_back(item);
  }
  return res;
}

// Function to build CLUEDataset
std::vector<std::shared_ptr<DatasetOp>> CLUEDataset::Build() {
  // A vector containing shared pointer to the Dataset Ops that this object will create
  std::vector<std::shared_ptr<DatasetOp>> node_ops;
  std::map<std::string, std::string> key_map;
  if (task_ == "AFQMC") {
    if (usage_ == "train") {
      key_map["sentence1"] = "sentence1";
      key_map["sentence2"] = "sentence2";
      key_map["label"] = "label";
    } else if (usage_ == "test") {
      key_map["id"] = "id";
      key_map["sentence1"] = "sentence1";
      key_map["sentence2"] = "sentence2";
    } else if (usage_ == "eval") {
      key_map["sentence1"] = "sentence1";
      key_map["sentence2"] = "sentence2";
      key_map["label"] = "label";
    }
  } else if (task_ == "CMNLI") {
    if (usage_ == "train") {
      key_map["sentence1"] = "sentence1";
      key_map["sentence2"] = "sentence2";
      key_map["label"] = "label";
    } else if (usage_ == "test") {
      key_map["id"] = "id";
      key_map["sentence1"] = "sentence1";
      key_map["sentence2"] = "sentence2";
    } else if (usage_ == "eval") {
      key_map["sentence1"] = "sentence1";
      key_map["sentence2"] = "sentence2";
      key_map["label"] = "label";
    }
  } else if (task_ == "CSL") {
    if (usage_ == "train") {
      key_map["id"] = "id";
      key_map["abst"] = "abst";
      key_map["keyword"] = "keyword";
      key_map["label"] = "label";
    } else if (usage_ == "test") {
      key_map["id"] = "id";
      key_map["abst"] = "abst";
      key_map["keyword"] = "keyword";
    } else if (usage_ == "eval") {
      key_map["id"] = "id";
      key_map["abst"] = "abst";
      key_map["keyword"] = "keyword";
      key_map["label"] = "label";
    }
  } else if (task_ == "IFLYTEK") {
    if (usage_ == "train") {
      key_map["label"] = "label";
      key_map["label_des"] = "label_des";
      key_map["sentence"] = "sentence";
    } else if (usage_ == "test") {
      key_map["id"] = "id";
      key_map["sentence"] = "sentence";
    } else if (usage_ == "eval") {
      key_map["label"] = "label";
      key_map["label_des"] = "label_des";
      key_map["sentence"] = "sentence";
    }
  } else if (task_ == "TNEWS") {
    if (usage_ == "train") {
      key_map["label"] = "label";
      key_map["label_desc"] = "label_desc";
      key_map["sentence"] = "sentence";
      key_map["keywords"] = "keywords";
    } else if (usage_ == "test") {
      key_map["id"] = "id";
      key_map["sentence"] = "sentence";
      key_map["keywords"] = "keywords";
    } else if (usage_ == "eval") {
      key_map["label"] = "label";
      key_map["label_desc"] = "label_desc";
      key_map["sentence"] = "sentence";
      key_map["keywords"] = "keywords";
    }
  } else if (task_ == "WSC") {
    if (usage_ == "train") {
      key_map["span1_index"] = "target/span1_index";
      key_map["span2_index"] = "target/span2_index";
      key_map["span1_text"] = "target/span1_text";
      key_map["span2_text"] = "target/span2_text";
      key_map["idx"] = "idx";
      key_map["label"] = "label";
      key_map["text"] = "text";
    } else if (usage_ == "test") {
      key_map["span1_index"] = "target/span1_index";
      key_map["span2_index"] = "target/span2_index";
      key_map["span1_text"] = "target/span1_text";
      key_map["span2_text"] = "target/span2_text";
      key_map["idx"] = "idx";
      key_map["text"] = "text";
    } else if (usage_ == "eval") {
      key_map["span1_index"] = "target/span1_index";
      key_map["span2_index"] = "target/span2_index";
      key_map["span1_text"] = "target/span1_text";
      key_map["span2_text"] = "target/span2_text";
      key_map["idx"] = "idx";
      key_map["label"] = "label";
      key_map["text"] = "text";
    }
  }

  ColKeyMap ck_map;
  for (auto &p : key_map) {
    ck_map.insert({p.first, split(p.second, '/')});
  }

  bool shuffle_files = (shuffle_ == ShuffleMode::kGlobal || shuffle_ == ShuffleMode::kFiles);

  // Sort the dataset files in a lexicographical order
  std::vector<std::string> sorted_dataset_files = dataset_files_;
  std::sort(sorted_dataset_files.begin(), sorted_dataset_files.end());

  std::shared_ptr<ClueOp> clue_op =
    std::make_shared<ClueOp>(num_workers_, rows_per_buffer_, num_samples_, worker_connector_size_, ck_map,
                             sorted_dataset_files, connector_que_size_, shuffle_files, num_shards_, shard_id_);
  RETURN_EMPTY_IF_ERROR(clue_op->Init());
  if (shuffle_ == ShuffleMode::kGlobal) {
    // Inject ShuffleOp
    std::shared_ptr<DatasetOp> shuffle_op = nullptr;
    int64_t num_rows = 0;

    // First, get the number of rows in the dataset
    RETURN_EMPTY_IF_ERROR(ClueOp::CountAllFileRows(sorted_dataset_files, &num_rows));

    // Add the shuffle op after this op
    RETURN_EMPTY_IF_ERROR(AddShuffleOp(sorted_dataset_files.size(), num_shards_, num_rows, 0, connector_que_size_,
                                       rows_per_buffer_, &shuffle_op));
    node_ops.push_back(shuffle_op);
  }

  node_ops.push_back(clue_op);
  return node_ops;
}

// Constructor for CocoDataset
CocoDataset::CocoDataset(const std::string &dataset_dir, const std::string &annotation_file, const std::string &task,
                         const bool &decode, const std::shared_ptr<SamplerObj> &sampler)
    : dataset_dir_(dataset_dir), annotation_file_(annotation_file), task_(task), decode_(decode), sampler_(sampler) {}

bool CocoDataset::ValidateParams() {
  if (!ValidateDatasetDirParam("CocoDataset", dataset_dir_)) {
    return false;
  }
  if (!ValidateDatasetSampler("CocoDataset", sampler_)) {
    return false;
  }
  Path annotation_file(annotation_file_);
  if (!annotation_file.Exists()) {
    MS_LOG(ERROR) << "annotation_file is invalid or not exist";
    return false;
  }
  std::set<std::string> task_list = {"Detection", "Stuff", "Panoptic", "Keypoint"};
  auto task_iter = task_list.find(task_);
  if (task_iter == task_list.end()) {
    MS_LOG(ERROR) << "Invalid task type";
    return false;
  }
  return true;
}

// Function to build CocoDataset
std::vector<std::shared_ptr<DatasetOp>> CocoDataset::Build() {
  // A vector containing shared pointer to the Dataset Ops that this object will create
  std::vector<std::shared_ptr<DatasetOp>> node_ops;

  CocoOp::TaskType task_type;
  if (task_ == "Detection") {
    task_type = CocoOp::TaskType::Detection;
  } else if (task_ == "Stuff") {
    task_type = CocoOp::TaskType::Stuff;
  } else if (task_ == "Keypoint") {
    task_type = CocoOp::TaskType::Keypoint;
  } else if (task_ == "Panoptic") {
    task_type = CocoOp::TaskType::Panoptic;
  }

  std::unique_ptr<DataSchema> schema = std::make_unique<DataSchema>();
  RETURN_EMPTY_IF_ERROR(
    schema->AddColumn(ColDescriptor(std::string("image"), DataType(DataType::DE_UINT8), TensorImpl::kFlexible, 1)));
  switch (task_type) {
    case CocoOp::TaskType::Detection:
      RETURN_EMPTY_IF_ERROR(schema->AddColumn(
        ColDescriptor(std::string("bbox"), DataType(DataType::DE_FLOAT32), TensorImpl::kFlexible, 1)));
      RETURN_EMPTY_IF_ERROR(schema->AddColumn(
        ColDescriptor(std::string("category_id"), DataType(DataType::DE_UINT32), TensorImpl::kFlexible, 1)));
      RETURN_EMPTY_IF_ERROR(schema->AddColumn(
        ColDescriptor(std::string("iscrowd"), DataType(DataType::DE_UINT32), TensorImpl::kFlexible, 1)));
      break;
    case CocoOp::TaskType::Stuff:
      RETURN_EMPTY_IF_ERROR(schema->AddColumn(
        ColDescriptor(std::string("segmentation"), DataType(DataType::DE_FLOAT32), TensorImpl::kFlexible, 1)));
      RETURN_EMPTY_IF_ERROR(schema->AddColumn(
        ColDescriptor(std::string("iscrowd"), DataType(DataType::DE_UINT32), TensorImpl::kFlexible, 1)));
      break;
    case CocoOp::TaskType::Keypoint:
      RETURN_EMPTY_IF_ERROR(schema->AddColumn(
        ColDescriptor(std::string("keypoints"), DataType(DataType::DE_FLOAT32), TensorImpl::kFlexible, 1)));
      RETURN_EMPTY_IF_ERROR(schema->AddColumn(
        ColDescriptor(std::string("num_keypoints"), DataType(DataType::DE_UINT32), TensorImpl::kFlexible, 1)));
      break;
    case CocoOp::TaskType::Panoptic:
      RETURN_EMPTY_IF_ERROR(schema->AddColumn(
        ColDescriptor(std::string("bbox"), DataType(DataType::DE_FLOAT32), TensorImpl::kFlexible, 1)));
      RETURN_EMPTY_IF_ERROR(schema->AddColumn(
        ColDescriptor(std::string("category_id"), DataType(DataType::DE_UINT32), TensorImpl::kFlexible, 1)));
      RETURN_EMPTY_IF_ERROR(schema->AddColumn(
        ColDescriptor(std::string("iscrowd"), DataType(DataType::DE_UINT32), TensorImpl::kFlexible, 1)));
      RETURN_EMPTY_IF_ERROR(
        schema->AddColumn(ColDescriptor(std::string("area"), DataType(DataType::DE_UINT32), TensorImpl::kFlexible, 1)));
      break;
    default:
      MS_LOG(ERROR) << "CocoDataset::Build : Invalid task type";
      return {};
  }
  std::shared_ptr<CocoOp> op =
    std::make_shared<CocoOp>(task_type, dataset_dir_, annotation_file_, num_workers_, rows_per_buffer_,
                             connector_que_size_, decode_, std::move(schema), std::move(sampler_->Build()));
  node_ops.push_back(op);
  return node_ops;
}

// Constructor for CSVDataset
CSVDataset::CSVDataset(const std::vector<std::string> &csv_files, char field_delim,
                       const std::vector<std::shared_ptr<CsvBase>> &column_defaults,
                       const std::vector<std::string> &column_names, int64_t num_samples, ShuffleMode shuffle,
                       int32_t num_shards, int32_t shard_id)
    : dataset_files_(csv_files),
      field_delim_(field_delim),
      column_defaults_(column_defaults),
      column_names_(column_names),
      num_samples_(num_samples),
      shuffle_(shuffle),
      num_shards_(num_shards),
      shard_id_(shard_id) {}

bool CSVDataset::ValidateParams() {
  if (!ValidateDatasetFilesParam("CSVDataset", dataset_files_)) {
    return false;
  }

  if (field_delim_ == '"' || field_delim_ == '\r' || field_delim_ == '\n') {
    MS_LOG(ERROR) << "CSVDataset: The field delimiter should not be \", \\r, \\n";
    return false;
  }

  if (num_samples_ < 0) {
    MS_LOG(ERROR) << "CSVDataset: Invalid number of samples: " << num_samples_;
    return false;
  }

  if (!ValidateDatasetShardParams("CSVDataset", num_shards_, shard_id_)) {
    return false;
  }

  if (find(column_defaults_.begin(), column_defaults_.end(), nullptr) != column_defaults_.end()) {
    MS_LOG(ERROR) << "CSVDataset: column_default should not be null.";
    return false;
  }

  if (!column_names_.empty()) {
    if (!ValidateDatasetColumnParam("CSVDataset", "column_names", column_names_)) {
      return false;
    }
  }

  return true;
}

// Function to build CSVDataset
std::vector<std::shared_ptr<DatasetOp>> CSVDataset::Build() {
  // A vector containing shared pointer to the Dataset Ops that this object will create
  std::vector<std::shared_ptr<DatasetOp>> node_ops;

  bool shuffle_files = (shuffle_ == ShuffleMode::kGlobal || shuffle_ == ShuffleMode::kFiles);

  // Sort the dataset files in a lexicographical order
  std::vector<std::string> sorted_dataset_files = dataset_files_;
  std::sort(sorted_dataset_files.begin(), sorted_dataset_files.end());

  std::vector<std::shared_ptr<CsvOp::BaseRecord>> column_default_list;
  for (auto v : column_defaults_) {
    if (v->type == CsvType::INT) {
      column_default_list.push_back(
        std::make_shared<CsvOp::Record<int>>(CsvOp::INT, std::dynamic_pointer_cast<CsvRecord<int>>(v)->value));
    } else if (v->type == CsvType::FLOAT) {
      column_default_list.push_back(
        std::make_shared<CsvOp::Record<float>>(CsvOp::FLOAT, std::dynamic_pointer_cast<CsvRecord<float>>(v)->value));
    } else if (v->type == CsvType::STRING) {
      column_default_list.push_back(std::make_shared<CsvOp::Record<std::string>>(
        CsvOp::STRING, std::dynamic_pointer_cast<CsvRecord<std::string>>(v)->value));
    }
  }

  std::shared_ptr<CsvOp> csv_op = std::make_shared<CsvOp>(
    sorted_dataset_files, field_delim_, column_default_list, column_names_, num_workers_, rows_per_buffer_,
    num_samples_, worker_connector_size_, connector_que_size_, shuffle_files, num_shards_, shard_id_);
  RETURN_EMPTY_IF_ERROR(csv_op->Init());
  if (shuffle_ == ShuffleMode::kGlobal) {
    // Inject ShuffleOp
    std::shared_ptr<DatasetOp> shuffle_op = nullptr;
    int64_t num_rows = 0;

    // First, get the number of rows in the dataset
    RETURN_EMPTY_IF_ERROR(CsvOp::CountAllFileRows(sorted_dataset_files, column_names_.empty(), &num_rows));

    // Add the shuffle op after this op
    RETURN_EMPTY_IF_ERROR(AddShuffleOp(sorted_dataset_files.size(), num_shards_, num_rows, 0, connector_que_size_,
                                       rows_per_buffer_, &shuffle_op));
    node_ops.push_back(shuffle_op);
  }

  node_ops.push_back(csv_op);
  return node_ops;
}

ImageFolderDataset::ImageFolderDataset(std::string dataset_dir, bool decode, std::shared_ptr<SamplerObj> sampler,
                                       bool recursive, std::set<std::string> extensions,
                                       std::map<std::string, int32_t> class_indexing)
    : dataset_dir_(dataset_dir),
      decode_(decode),
      sampler_(sampler),
      recursive_(recursive),
      class_indexing_(class_indexing),
      exts_(extensions) {}

bool ImageFolderDataset::ValidateParams() {
  return ValidateDatasetDirParam("ImageFolderDataset", dataset_dir_) &&
         ValidateDatasetSampler("ImageFolderDataset", sampler_);
}

std::vector<std::shared_ptr<DatasetOp>> ImageFolderDataset::Build() {
  // A vector containing shared pointer to the Dataset Ops that this object will create
  std::vector<std::shared_ptr<DatasetOp>> node_ops;

  // Do internal Schema generation.
  // This arg is exist in ImageFolderOp, but not externalized (in Python API).
  std::unique_ptr<DataSchema> schema = std::make_unique<DataSchema>();
  TensorShape scalar = TensorShape::CreateScalar();
  RETURN_EMPTY_IF_ERROR(
    schema->AddColumn(ColDescriptor("image", DataType(DataType::DE_UINT8), TensorImpl::kFlexible, 1)));
  RETURN_EMPTY_IF_ERROR(
    schema->AddColumn(ColDescriptor("label", DataType(DataType::DE_INT32), TensorImpl::kFlexible, 0, &scalar)));
  node_ops.push_back(std::make_shared<ImageFolderOp>(num_workers_, rows_per_buffer_, dataset_dir_, connector_que_size_,
                                                     recursive_, decode_, exts_, class_indexing_, std::move(schema),
                                                     std::move(sampler_->Build())));
  return node_ops;
}

#ifndef ENABLE_ANDROID
ManifestDataset::ManifestDataset(const std::string &dataset_file, const std::string &usage,
                                 const std::shared_ptr<SamplerObj> &sampler,
                                 const std::map<std::string, int32_t> &class_indexing, bool decode)
    : dataset_file_(dataset_file), usage_(usage), decode_(decode), class_index_(class_indexing), sampler_(sampler) {}

bool ManifestDataset::ValidateParams() {
  std::vector<char> forbidden_symbols = {':', '*', '?', '"', '<', '>', '|', '`', '&', '\'', ';'};
  for (char c : dataset_file_) {
    auto p = std::find(forbidden_symbols.begin(), forbidden_symbols.end(), c);
    if (p != forbidden_symbols.end()) {
      MS_LOG(ERROR) << "filename should not contains :*?\"<>|`&;\'";
      return false;
    }
  }

  Path manifest_file(dataset_file_);
  if (!manifest_file.Exists()) {
    MS_LOG(ERROR) << "dataset file: [" << dataset_file_ << "] is invalid or not exist";
    return false;
  }

  if (!ValidateDatasetSampler("ManifestDataset", sampler_)) {
    return false;
  }

  std::vector<std::string> usage_list = {"train", "eval", "inference"};
  if (find(usage_list.begin(), usage_list.end(), usage_) == usage_list.end()) {
    MS_LOG(ERROR) << "usage should be train, eval or inference.";
    return false;
  }

  return true;
}

std::vector<std::shared_ptr<DatasetOp>> ManifestDataset::Build() {
  // A vector containing shared pointer to the Dataset Ops that this object will create
  std::vector<std::shared_ptr<DatasetOp>> node_ops;

  // Do internal Schema generation.
  auto schema = std::make_unique<DataSchema>();
  RETURN_EMPTY_IF_ERROR(schema->AddColumn(ColDescriptor("image", DataType(DataType::DE_UINT8), TensorImpl::kCv, 1)));
  TensorShape scalar = TensorShape::CreateScalar();
  RETURN_EMPTY_IF_ERROR(
    schema->AddColumn(ColDescriptor("label", DataType(DataType::DE_UINT32), TensorImpl::kFlexible, 0, &scalar)));

  std::shared_ptr<ManifestOp> manifest_op;
  manifest_op =
    std::make_shared<ManifestOp>(num_workers_, rows_per_buffer_, dataset_file_, connector_que_size_, decode_,
                                 class_index_, std::move(schema), std::move(sampler_->Build()), usage_);

  node_ops.push_back(manifest_op);
  return node_ops;
}
#endif

MnistDataset::MnistDataset(std::string dataset_dir, std::string usage, std::shared_ptr<SamplerObj> sampler)
    : dataset_dir_(dataset_dir), usage_(usage), sampler_(sampler) {}

bool MnistDataset::ValidateParams() {
  return ValidateStringValue(usage_, {"train", "test", "all"}) &&
         ValidateDatasetDirParam("MnistDataset", dataset_dir_) && ValidateDatasetSampler("MnistDataset", sampler_);
}

std::vector<std::shared_ptr<DatasetOp>> MnistDataset::Build() {
  // A vector containing shared pointer to the Dataset Ops that this object will create
  std::vector<std::shared_ptr<DatasetOp>> node_ops;

  // Do internal Schema generation.
  auto schema = std::make_unique<DataSchema>();
  RETURN_EMPTY_IF_ERROR(schema->AddColumn(ColDescriptor("image", DataType(DataType::DE_UINT8), TensorImpl::kCv, 1)));
  TensorShape scalar = TensorShape::CreateScalar();
  RETURN_EMPTY_IF_ERROR(
    schema->AddColumn(ColDescriptor("label", DataType(DataType::DE_UINT32), TensorImpl::kFlexible, 0, &scalar)));

  node_ops.push_back(std::make_shared<MnistOp>(usage_, num_workers_, rows_per_buffer_, dataset_dir_,
                                               connector_que_size_, std::move(schema), std::move(sampler_->Build())));
  return node_ops;
}

// ValideParams for RandomDataset
bool RandomDataset::ValidateParams() {
  if (total_rows_ < 0) {
    MS_LOG(ERROR) << "RandomDataset: total_rows must be greater than or equal 0, now get " << total_rows_;
    return false;
  }
  if (!ValidateDatasetSampler("RandomDataset", sampler_)) {
    return false;
  }
  if (!columns_list_.empty()) {
    if (!ValidateDatasetColumnParam("RandomDataset", "columns_list", columns_list_)) {
      return false;
    }
  }
  return true;
}

int32_t RandomDataset::GenRandomInt(int32_t min, int32_t max) {
  std::uniform_int_distribution<int32_t> uniDist(min, max);
  return uniDist(rand_gen_);
}

// Build for RandomDataset
std::vector<std::shared_ptr<DatasetOp>> RandomDataset::Build() {
  // A vector containing shared pointer to the Dataset Ops that this object will create
  std::vector<std::shared_ptr<DatasetOp>> node_ops;

  rand_gen_.seed(GetSeed());  // seed the random generator
  // If total rows was not given, then randomly pick a number
  std::shared_ptr<SchemaObj> schema_obj;
  if (!schema_path_.empty()) {
    schema_obj = Schema(schema_path_);
    if (schema_obj == nullptr) {
      return {};
    }
  }

  std::string schema_json_string, schema_file_path;
  if (schema_ != nullptr) {
    schema_->set_dataset_type("Random");
    if (total_rows_ != 0) {
      schema_->set_num_rows(total_rows_);
    }
    schema_json_string = schema_->to_json();
  } else {
    schema_file_path = schema_path_;
  }

  std::unique_ptr<DataSchema> data_schema;
  std::vector<std::string> columns_to_load;
  if (columns_list_.size() > 0) {
    columns_to_load = columns_list_;
  }
  if (!schema_file_path.empty() || !schema_json_string.empty()) {
    data_schema = std::make_unique<DataSchema>();
    if (!schema_file_path.empty()) {
      data_schema->LoadSchemaFile(schema_file_path, columns_to_load);
    } else if (!schema_json_string.empty()) {
      data_schema->LoadSchemaString(schema_json_string, columns_to_load);
    }
  }
  std::shared_ptr<RandomDataOp> op;
  op = std::make_shared<RandomDataOp>(num_workers_, connector_que_size_, rows_per_buffer_, total_rows_,
                                      std::move(data_schema), std::move(sampler_->Build()));
  node_ops.push_back(op);
  return node_ops;
}

// Constructor for TextFileDataset
TextFileDataset::TextFileDataset(std::vector<std::string> dataset_files, int32_t num_samples, ShuffleMode shuffle,
                                 int32_t num_shards, int32_t shard_id)
    : dataset_files_(dataset_files),
      num_samples_(num_samples),
      shuffle_(shuffle),
      num_shards_(num_shards),
      shard_id_(shard_id) {}

bool TextFileDataset::ValidateParams() {
  if (!ValidateDatasetFilesParam("TextFileDataset", dataset_files_)) {
    return false;
  }

  if (num_samples_ < 0) {
    MS_LOG(ERROR) << "TextFileDataset: Invalid number of samples: " << num_samples_;
    return false;
  }

  if (!ValidateDatasetShardParams("TextFileDataset", num_shards_, shard_id_)) {
    return false;
  }

  return true;
}

// Function to build TextFileDataset
std::vector<std::shared_ptr<DatasetOp>> TextFileDataset::Build() {
  // A vector containing shared pointer to the Dataset Ops that this object will create
  std::vector<std::shared_ptr<DatasetOp>> node_ops;

  bool shuffle_files = (shuffle_ == ShuffleMode::kGlobal || shuffle_ == ShuffleMode::kFiles);

  // Sort the dataset files in a lexicographical order
  std::vector<std::string> sorted_dataset_files = dataset_files_;
  std::sort(sorted_dataset_files.begin(), sorted_dataset_files.end());

  // Do internal Schema generation.
  auto schema = std::make_unique<DataSchema>();
  RETURN_EMPTY_IF_ERROR(
    schema->AddColumn(ColDescriptor("text", DataType(DataType::DE_UINT8), TensorImpl::kFlexible, 1)));

  // Create and initalize TextFileOp
  std::shared_ptr<TextFileOp> text_file_op = std::make_shared<TextFileOp>(
    num_workers_, rows_per_buffer_, num_samples_, worker_connector_size_, std::move(schema), sorted_dataset_files,
    connector_que_size_, shuffle_files, num_shards_, shard_id_, std::move(nullptr));
  RETURN_EMPTY_IF_ERROR(text_file_op->Init());

  if (shuffle_ == ShuffleMode::kGlobal) {
    // Inject ShuffleOp
    std::shared_ptr<DatasetOp> shuffle_op = nullptr;
    int64_t num_rows = 0;

    // First, get the number of rows in the dataset
    RETURN_EMPTY_IF_ERROR(TextFileOp::CountAllFileRows(sorted_dataset_files, &num_rows));

    // Add the shuffle op after this op
    RETURN_EMPTY_IF_ERROR(AddShuffleOp(sorted_dataset_files.size(), num_shards_, num_rows, 0, connector_que_size_,
                                       rows_per_buffer_, &shuffle_op));
    node_ops.push_back(shuffle_op);
  }

  // Add TextFileOp
  node_ops.push_back(text_file_op);
  return node_ops;
}

#ifndef ENABLE_ANDROID
// Validator for TFRecordDataset
bool TFRecordDataset::ValidateParams() { return true; }

// Function to build TFRecordDataset
std::vector<std::shared_ptr<DatasetOp>> TFRecordDataset::Build() {
  // A vector containing shared pointer to the Dataset Ops that this object will create
  std::vector<std::shared_ptr<DatasetOp>> node_ops;

  // Sort the datasets file in a lexicographical order
  std::vector<std::string> sorted_dir_files = dataset_files_;
  std::sort(sorted_dir_files.begin(), sorted_dir_files.end());

  // Create Schema Object
  std::unique_ptr<DataSchema> data_schema = std::make_unique<DataSchema>();
  if (!schema_path_.empty()) {
    RETURN_EMPTY_IF_ERROR(data_schema->LoadSchemaFile(schema_path_, columns_list_));
  } else if (schema_obj_ != nullptr) {
    std::string schema_json_string = schema_obj_->to_json();
    RETURN_EMPTY_IF_ERROR(data_schema->LoadSchemaString(schema_json_string, columns_list_));
  }

  bool shuffle_files = (shuffle_ == ShuffleMode::kGlobal || shuffle_ == ShuffleMode::kFiles);

  // Create and initalize TFReaderOp
  std::shared_ptr<TFReaderOp> tf_reader_op = std::make_shared<TFReaderOp>(
    num_workers_, worker_connector_size_, rows_per_buffer_, num_samples_, sorted_dir_files, std::move(data_schema),
    connector_que_size_, columns_list_, shuffle_files, num_shards_, shard_id_, shard_equal_rows_, nullptr);

  RETURN_EMPTY_IF_ERROR(tf_reader_op->Init());

  if (shuffle_ == ShuffleMode::kGlobal) {
    // Inject ShuffleOp

    std::shared_ptr<DatasetOp> shuffle_op = nullptr;
    int64_t num_rows = 0;

    // First, get the number of rows in the dataset
    RETURN_EMPTY_IF_ERROR(TFReaderOp::CountTotalRows(&num_rows, sorted_dir_files));

    // Add the shuffle op after this op
    RETURN_EMPTY_IF_ERROR(AddShuffleOp(sorted_dir_files.size(), num_shards_, num_rows, 0, connector_que_size_,
                                       rows_per_buffer_, &shuffle_op));
    node_ops.push_back(shuffle_op);
  }

  // Add TFReaderOp
  node_ops.push_back(tf_reader_op);
  return node_ops;
}

// Constructor for VOCDataset
VOCDataset::VOCDataset(const std::string &dataset_dir, const std::string &task, const std::string &usage,
                       const std::map<std::string, int32_t> &class_indexing, bool decode,
                       std::shared_ptr<SamplerObj> sampler)
    : dataset_dir_(dataset_dir),
      task_(task),
      usage_(usage),
      class_index_(class_indexing),
      decode_(decode),
      sampler_(sampler) {}

bool VOCDataset::ValidateParams() {
  Path dir(dataset_dir_);
  if (!dir.IsDirectory()) {
    MS_LOG(ERROR) << "Invalid dataset path or no dataset path is specified.";
    return false;
  }
  if (!ValidateDatasetSampler("VOCDataset", sampler_)) {
    return false;
  }
  if (task_ == "Segmentation") {
    if (!class_index_.empty()) {
      MS_LOG(ERROR) << "class_indexing is invalid in Segmentation task.";
      return false;
    }
    Path imagesets_file = dir / "ImageSets" / "Segmentation" / usage_ + ".txt";
    if (!imagesets_file.Exists()) {
      MS_LOG(ERROR) << "Invalid mode: " << usage_ << ", file \"" << imagesets_file << "\" does not exist!";
      return false;
    }
  } else if (task_ == "Detection") {
    Path imagesets_file = dir / "ImageSets" / "Main" / usage_ + ".txt";
    if (!imagesets_file.Exists()) {
      MS_LOG(ERROR) << "Invalid mode: " << usage_ << ", file \"" << imagesets_file << "\" does not exist!";
      return false;
    }
  } else {
    MS_LOG(ERROR) << "Invalid task: " << task_;
    return false;
  }
  return true;
}

// Function to build VOCDataset
std::vector<std::shared_ptr<DatasetOp>> VOCDataset::Build() {
  // A vector containing shared pointer to the Dataset Ops that this object will create
  std::vector<std::shared_ptr<DatasetOp>> node_ops;

  auto schema = std::make_unique<DataSchema>();
  VOCOp::TaskType task_type_;

  if (task_ == "Segmentation") {
    task_type_ = VOCOp::TaskType::Segmentation;
    RETURN_EMPTY_IF_ERROR(schema->AddColumn(
      ColDescriptor(std::string(kColumnImage), DataType(DataType::DE_UINT8), TensorImpl::kFlexible, 1)));
    RETURN_EMPTY_IF_ERROR(schema->AddColumn(
      ColDescriptor(std::string(kColumnTarget), DataType(DataType::DE_UINT8), TensorImpl::kFlexible, 1)));
  } else if (task_ == "Detection") {
    task_type_ = VOCOp::TaskType::Detection;
    RETURN_EMPTY_IF_ERROR(schema->AddColumn(
      ColDescriptor(std::string(kColumnImage), DataType(DataType::DE_UINT8), TensorImpl::kFlexible, 1)));
    RETURN_EMPTY_IF_ERROR(schema->AddColumn(
      ColDescriptor(std::string(kColumnBbox), DataType(DataType::DE_FLOAT32), TensorImpl::kFlexible, 1)));
    RETURN_EMPTY_IF_ERROR(schema->AddColumn(
      ColDescriptor(std::string(kColumnLabel), DataType(DataType::DE_UINT32), TensorImpl::kFlexible, 1)));
    RETURN_EMPTY_IF_ERROR(schema->AddColumn(
      ColDescriptor(std::string(kColumnDifficult), DataType(DataType::DE_UINT32), TensorImpl::kFlexible, 1)));
    RETURN_EMPTY_IF_ERROR(schema->AddColumn(
      ColDescriptor(std::string(kColumnTruncate), DataType(DataType::DE_UINT32), TensorImpl::kFlexible, 1)));
  }

  std::shared_ptr<VOCOp> voc_op;
  voc_op = std::make_shared<VOCOp>(task_type_, usage_, dataset_dir_, class_index_, num_workers_, rows_per_buffer_,
                                   connector_que_size_, decode_, std::move(schema), std::move(sampler_->Build()));
  node_ops.push_back(voc_op);
  return node_ops;
}
#endif

// DERIVED DATASET CLASSES LEAF-NODE DATASETS
// (In alphabetical order)

BatchDataset::BatchDataset(int32_t batch_size, bool drop_remainder, bool pad, std::vector<std::string> cols_to_map,
                           std::map<std::string, std::pair<TensorShape, std::shared_ptr<Tensor>>> pad_map)
    : batch_size_(batch_size),
      drop_remainder_(drop_remainder),
      pad_(pad),
      cols_to_map_(cols_to_map),
      pad_map_(pad_map) {}

std::vector<std::shared_ptr<DatasetOp>> BatchDataset::Build() {
  // A vector containing shared pointer to the Dataset Ops that this object will create
  std::vector<std::shared_ptr<DatasetOp>> node_ops;

#ifdef ENABLE_PYTHON
  py::function noop;
  node_ops.push_back(std::make_shared<BatchOp>(batch_size_, drop_remainder_, pad_, connector_que_size_, num_workers_,
                                               cols_to_map_, noop, noop, pad_map_));
#else
  node_ops.push_back(std::make_shared<BatchOp>(batch_size_, drop_remainder_, pad_, connector_que_size_, num_workers_,
                                               cols_to_map_, pad_map_));
#endif
  return node_ops;
}

bool BatchDataset::ValidateParams() {
  if (batch_size_ <= 0) {
    MS_LOG(ERROR) << "Batch: batch_size should be positive integer, but got: " << batch_size_;
    return false;
  }

  return true;
}

#ifndef ENABLE_ANDROID
BucketBatchByLengthDataset::BucketBatchByLengthDataset(
  const std::vector<std::string> &column_names, const std::vector<int32_t> &bucket_boundaries,
  const std::vector<int32_t> &bucket_batch_sizes, TensorRow (*element_length_function)(TensorRow),
  const std::map<std::string, std::pair<TensorShape, std::shared_ptr<Tensor>>> &pad_info, bool pad_to_bucket_boundary,
  bool drop_remainder)
    : column_names_(column_names),
      bucket_boundaries_(bucket_boundaries),
      bucket_batch_sizes_(bucket_batch_sizes),
      element_length_function_(element_length_function),
      pad_info_(pad_info),
      pad_to_bucket_boundary_(pad_to_bucket_boundary),
      drop_remainder_(drop_remainder) {}

std::vector<std::shared_ptr<DatasetOp>> BucketBatchByLengthDataset::Build() {
  // A vector containing shared pointer to the Dataset Ops that this object will create
  std::vector<std::shared_ptr<DatasetOp>> node_ops;

  std::shared_ptr<TensorOp> c_func;
  if (element_length_function_ != nullptr) {
    c_func = std::make_shared<CFuncOp>(element_length_function_);
  } else {
    c_func = nullptr;
  }
  node_ops.push_back(std::make_shared<BucketBatchByLengthOp>(column_names_, bucket_boundaries_, bucket_batch_sizes_,
                                                             c_func, pad_info_, pad_to_bucket_boundary_,
                                                             drop_remainder_, connector_que_size_));
  return node_ops;
}

bool BucketBatchByLengthDataset::ValidateParams() {
  if (element_length_function_ == nullptr && column_names_.size() != 1) {
    MS_LOG(ERROR) << "BucketBatchByLength: If element_length_function is not specified, exactly one column name "
                     "should be passed.";
    return false;
  }

  // Check bucket_boundaries: must be positive and strictly increasing
  if (bucket_boundaries_.empty()) {
    MS_LOG(ERROR) << "BucketBatchByLength: bucket_boundaries cannot be empty.";
    return false;
  }
  for (int i = 0; i < bucket_boundaries_.size(); i++) {
    if (bucket_boundaries_[i] <= 0) {
      MS_LOG(ERROR)
        << "BucketBatchByLength: bucket_boundaries must only contain positive numbers. However, the element at index: "
        << i << " was: " << bucket_boundaries_[i];
      return false;
    }
    if (i > 0 && bucket_boundaries_[i - 1] >= bucket_boundaries_[i]) {
      MS_LOG(ERROR)
        << "BucketBatchByLength: bucket_boundaries must be strictly increasing. However, the elements at index: "
        << i - 1 << " and " << i << " were: " << bucket_boundaries_[i - 1] << " and " << bucket_boundaries_[i]
        << " respectively.";
      return false;
    }
  }

  // Check bucket_batch_sizes: must be positive
  if (bucket_batch_sizes_.empty()) {
    MS_LOG(ERROR) << "BucketBatchByLength: bucket_batch_sizes must be non-empty";
    return false;
  }
  if (bucket_batch_sizes_.size() != bucket_boundaries_.size() + 1) {
    MS_LOG(ERROR) << "BucketBatchByLength: bucket_batch_sizes's size must equal the size of bucket_boundaries + 1";
    return false;
  }
  if (std::any_of(bucket_batch_sizes_.begin(), bucket_batch_sizes_.end(), [](int i) { return i <= 0; })) {
    MS_LOG(ERROR) << "BucketBatchByLength: bucket_batch_sizes must only contain positive numbers.";
    return false;
  }
  return true;
}

BuildVocabDataset::BuildVocabDataset(std::shared_ptr<Vocab> vocab, const std::vector<std::string> &columns,
                                     const std::pair<int64_t, int64_t> &freq_range, int64_t top_k,
                                     const std::vector<std::string> &special_tokens, bool special_first)
    : vocab_(vocab),
      columns_(columns),
      freq_range_(freq_range),
      top_k_(top_k),
      special_tokens_(special_tokens),
      special_first_(special_first) {}

// Function to build BuildVocabDataset
std::vector<std::shared_ptr<DatasetOp>> BuildVocabDataset::Build() {
  // A vector containing shared pointer to the Dataset Ops that this object will create
  std::vector<std::shared_ptr<DatasetOp>> node_ops;

  std::shared_ptr<BuildVocabOp> build_vocab_op;
  build_vocab_op = std::make_shared<BuildVocabOp>(vocab_, columns_, freq_range_, top_k_, special_tokens_,
                                                  special_first_, num_workers_, connector_que_size_);
  node_ops.push_back(build_vocab_op);
  return node_ops;
}

bool BuildVocabDataset::ValidateParams() {
  if (vocab_ == nullptr) {
    MS_LOG(ERROR) << "BuildVocab: vocab is null.";
    return false;
  }
  if (top_k_ <= 0) {
    MS_LOG(ERROR) << "BuildVocab: top_k shoule be positive, but got: " << top_k_;
    return false;
  }
  if (freq_range_.first < 0 || freq_range_.second > kDeMaxFreq || freq_range_.first > freq_range_.second) {
    MS_LOG(ERROR) << "BuildVocab: requency_range [a,b] should be 0 <= a <= b (a,b are inclusive), "
                  << "but got [" << freq_range_.first << ", " << freq_range_.second << "]";
    return false;
  }
  if (!columns_.empty()) {
    if (!ValidateDatasetColumnParam("BuildVocab", "columns", columns_)) {
      return false;
    }
  }
  return true;
}
#endif

// Function to build ConcatOp
ConcatDataset::ConcatDataset(const std::vector<std::shared_ptr<Dataset>> &datasets) : datasets_(datasets) {
  this->children = datasets_;
}

bool ConcatDataset::ValidateParams() {
  if (datasets_.empty()) {
    MS_LOG(ERROR) << "Concat: concatenated datasets are not specified.";
    return false;
  }
  if (find(datasets_.begin(), datasets_.end(), nullptr) != datasets_.end()) {
    MS_LOG(ERROR) << "Concat: concatenated dataset should not be null.";
    return false;
  }
  return true;
}

std::vector<std::shared_ptr<DatasetOp>> ConcatDataset::Build() {
  // A vector containing shared pointer to the Dataset Ops that this object will create
  std::vector<std::shared_ptr<DatasetOp>> node_ops;

  node_ops.push_back(std::make_shared<ConcatOp>(connector_que_size_));
  return node_ops;
}

MapDataset::MapDataset(std::vector<std::shared_ptr<TensorOperation>> operations, std::vector<std::string> input_columns,
                       std::vector<std::string> output_columns, const std::vector<std::string> &project_columns)
    : operations_(operations),
      input_columns_(input_columns),
      output_columns_(output_columns),
      project_columns_(project_columns) {}

std::vector<std::shared_ptr<DatasetOp>> MapDataset::Build() {
  // A vector containing shared pointer to the Dataset Ops that this object will create
  std::vector<std::shared_ptr<DatasetOp>> node_ops;

  std::vector<std::shared_ptr<TensorOp>> tensor_ops;

  // Build tensorOp from tensorOperation vector
  // This is to ensure each iterator hold its own copy of the tensorOp objects.
  (void)std::transform(
    operations_.begin(), operations_.end(), std::back_inserter(tensor_ops),
    [](std::shared_ptr<TensorOperation> operation) -> std::shared_ptr<TensorOp> { return operation->Build(); });

  // This parameter will be removed with next rebase
  std::vector<std::string> col_orders;
  auto map_op = std::make_shared<MapOp>(input_columns_, output_columns_, tensor_ops, num_workers_, connector_que_size_);
  if (!project_columns_.empty()) {
    auto project_op = std::make_shared<ProjectOp>(project_columns_);
    node_ops.push_back(project_op);
  }

  node_ops.push_back(map_op);
  return node_ops;
}

bool MapDataset::ValidateParams() {
  if (operations_.empty()) {
    MS_LOG(ERROR) << "Map: No operation is specified.";
    return false;
  }
  if (!input_columns_.empty()) {
    if (!ValidateDatasetColumnParam("MapDataset", "input_columns", input_columns_)) {
      return false;
    }
  }
  if (!output_columns_.empty()) {
    if (!ValidateDatasetColumnParam("MapDataset", "output_columns", output_columns_)) {
      return false;
    }
  }
  if (!project_columns_.empty()) {
    if (!ValidateDatasetColumnParam("MapDataset", "project_columns", project_columns_)) {
      return false;
    }
  }
  return true;
}

// Function to build ProjectOp
ProjectDataset::ProjectDataset(const std::vector<std::string> &columns) : columns_(columns) {}

bool ProjectDataset::ValidateParams() {
  if (columns_.empty()) {
    MS_LOG(ERROR) << "ProjectDataset: No columns are specified.";
    return false;
  }
  if (!ValidateDatasetColumnParam("ProjectDataset", "columns", columns_)) {
    return false;
  }
  return true;
}

std::vector<std::shared_ptr<DatasetOp>> ProjectDataset::Build() {
  // A vector containing shared pointer to the Dataset Ops that this object will create
  std::vector<std::shared_ptr<DatasetOp>> node_ops;

  node_ops.push_back(std::make_shared<ProjectOp>(columns_));
  return node_ops;
}

// Function to build RenameOp
RenameDataset::RenameDataset(const std::vector<std::string> &input_columns,
                             const std::vector<std::string> &output_columns)
    : input_columns_(input_columns), output_columns_(output_columns) {}

bool RenameDataset::ValidateParams() {
  if (input_columns_.size() != output_columns_.size()) {
    MS_LOG(ERROR) << "RenameDataset: input and output columns must be the same size";
    return false;
  }
  if (!ValidateDatasetColumnParam("RenameDataset", "input_columns", input_columns_) ||
      !ValidateDatasetColumnParam("RenameDataset", "output_columns", output_columns_)) {
    return false;
  }
  return true;
}

std::vector<std::shared_ptr<DatasetOp>> RenameDataset::Build() {
  // A vector containing shared pointer to the Dataset Ops that this object will create
  std::vector<std::shared_ptr<DatasetOp>> node_ops;

  node_ops.push_back(std::make_shared<RenameOp>(input_columns_, output_columns_, connector_que_size_));
  return node_ops;
}

RepeatDataset::RepeatDataset(int32_t count) : repeat_count_(count) {}

std::vector<std::shared_ptr<DatasetOp>> RepeatDataset::Build() {
  // A vector containing shared pointer to the Dataset Ops that this object will create
  std::vector<std::shared_ptr<DatasetOp>> node_ops;

  node_ops.push_back(std::make_shared<RepeatOp>(repeat_count_));
  return node_ops;
}

bool RepeatDataset::ValidateParams() {
  if (repeat_count_ <= 0 && repeat_count_ != -1) {
    MS_LOG(ERROR) << "Repeat: repeat_count should be either -1 or positive integer, repeat_count_: " << repeat_count_;
    return false;
  }

  return true;
}

// Constructor for ShuffleDataset
ShuffleDataset::ShuffleDataset(int32_t shuffle_size, bool reset_every_epoch)
    : shuffle_size_(shuffle_size), shuffle_seed_(GetSeed()), reset_every_epoch_(reset_every_epoch) {}

// Function to build the ShuffleOp
std::vector<std::shared_ptr<DatasetOp>> ShuffleDataset::Build() {
  // A vector containing shared pointer to the Dataset Ops that this object will create
  std::vector<std::shared_ptr<DatasetOp>> node_ops;

  node_ops.push_back(std::make_shared<ShuffleOp>(shuffle_size_, shuffle_seed_, connector_que_size_, reset_every_epoch_,
                                                 rows_per_buffer_));
  return node_ops;
}

// Function to validate the parameters for ShuffleDataset
bool ShuffleDataset::ValidateParams() {
  if (shuffle_size_ <= 1) {
    MS_LOG(ERROR) << "ShuffleDataset: Invalid input, shuffle_size: " << shuffle_size_;
    return false;
  }

  return true;
}

// Constructor for SkipDataset
SkipDataset::SkipDataset(int32_t count) : skip_count_(count) {}

// Function to build the SkipOp
std::vector<std::shared_ptr<DatasetOp>> SkipDataset::Build() {
  // A vector containing shared pointer to the Dataset Ops that this object will create
  std::vector<std::shared_ptr<DatasetOp>> node_ops;

  node_ops.push_back(std::make_shared<SkipOp>(skip_count_, connector_que_size_));
  return node_ops;
}

// Function to validate the parameters for SkipDataset
bool SkipDataset::ValidateParams() {
  if (skip_count_ <= -1) {
    MS_LOG(ERROR) << "Skip: skip_count should not be negative, skip_count: " << skip_count_;
    return false;
  }

  return true;
}

// Constructor for TakeDataset
TakeDataset::TakeDataset(int32_t count) : take_count_(count) {}

// Function to build the TakeOp
std::vector<std::shared_ptr<DatasetOp>> TakeDataset::Build() {
  // A vector containing shared pointer to the Dataset Ops that this object will create
  std::vector<std::shared_ptr<DatasetOp>> node_ops;

  node_ops.push_back(std::make_shared<TakeOp>(take_count_, connector_que_size_));
  return node_ops;
}

// Function to validate the parameters for TakeDataset
bool TakeDataset::ValidateParams() {
  if (take_count_ <= 0 && take_count_ != -1) {
    MS_LOG(ERROR) << "Take: take_count should be either -1 or positive integer, take_count: " << take_count_;
    return false;
  }

  return true;
}

// Function to build ZipOp
ZipDataset::ZipDataset(const std::vector<std::shared_ptr<Dataset>> &datasets) : datasets_(datasets) {
  for (auto dataset : datasets_) {
    this->children.push_back(dataset);
  }
}

bool ZipDataset::ValidateParams() {
  if (datasets_.empty()) {
    MS_LOG(ERROR) << "Zip: dataset to zip are not specified.";
    return false;
  }
  if (find(datasets_.begin(), datasets_.end(), nullptr) != datasets_.end()) {
    MS_LOG(ERROR) << "ZipDataset: zip dataset should not be null.";
    return false;
  }
  return true;
}

std::vector<std::shared_ptr<DatasetOp>> ZipDataset::Build() {
  // A vector containing shared pointer to the Dataset Ops that this object will create
  std::vector<std::shared_ptr<DatasetOp>> node_ops;

  node_ops.push_back(std::make_shared<ZipOp>(rows_per_buffer_, connector_que_size_));
  return node_ops;
}

}  // namespace api
}  // namespace dataset
}  // namespace mindspore
