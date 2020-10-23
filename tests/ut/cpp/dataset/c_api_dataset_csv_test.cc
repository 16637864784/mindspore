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
#include "common/common.h"
#include "minddata/dataset/core/config_manager.h"
#include "minddata/dataset/core/global_context.h"
#include "minddata/dataset/include/datasets.h"

// IR non-leaf nodes
#include "minddata/dataset/engine/ir/datasetops/batch_node.h"
#include "minddata/dataset/engine/ir/datasetops/bucket_batch_by_length_node.h"
#include "minddata/dataset/engine/ir/datasetops/build_vocab_node.h"
#include "minddata/dataset/engine/ir/datasetops/concat_node.h"
#include "minddata/dataset/engine/ir/datasetops/map_node.h"
#include "minddata/dataset/engine/ir/datasetops/project_node.h"
#include "minddata/dataset/engine/ir/datasetops/rename_node.h"
#include "minddata/dataset/engine/ir/datasetops/repeat_node.h"
#include "minddata/dataset/engine/ir/datasetops/shuffle_node.h"
#include "minddata/dataset/engine/ir/datasetops/skip_node.h"
#include "minddata/dataset/engine/ir/datasetops/take_node.h"
#include "minddata/dataset/engine/ir/datasetops/zip_node.h"

// IR leaf nodes
#include "minddata/dataset/engine/ir/datasetops/source/album_node.h"
#include "minddata/dataset/engine/ir/datasetops/source/celeba_node.h"
#include "minddata/dataset/engine/ir/datasetops/source/cifar100_node.h"
#include "minddata/dataset/engine/ir/datasetops/source/cifar10_node.h"
#include "minddata/dataset/engine/ir/datasetops/source/clue_node.h"
#include "minddata/dataset/engine/ir/datasetops/source/coco_node.h"
#include "minddata/dataset/engine/ir/datasetops/source/csv_node.h"
#include "minddata/dataset/engine/ir/datasetops/source/image_folder_node.h"
#include "minddata/dataset/engine/ir/datasetops/source/manifest_node.h"
#include "minddata/dataset/engine/ir/datasetops/source/minddata_node.h"
#include "minddata/dataset/engine/ir/datasetops/source/random_node.h"
#include "minddata/dataset/engine/ir/datasetops/source/text_file_node.h"
#include "minddata/dataset/engine/ir/datasetops/source/tf_record_node.h"
#include "minddata/dataset/engine/ir/datasetops/source/voc_node.h"

using namespace mindspore::dataset::api;
using mindspore::dataset::GlobalContext;
using mindspore::dataset::ShuffleMode;
using mindspore::dataset::Tensor;

class MindDataTestPipeline : public UT::DatasetOpTesting {
 protected:
};

TEST_F(MindDataTestPipeline, TestCSVDatasetBasic) {
  MS_LOG(INFO) << "Doing MindDataTestPipeline-TestCSVDatasetBasic.";

  // Create a CSVDataset, with single CSV file
  std::string train_file = datasets_root_path_ + "/testCSV/1.csv";
  std::vector<std::string> column_names = {"col1", "col2", "col3", "col4"};
  std::shared_ptr<Dataset> ds = CSV({train_file}, ',', {}, column_names, 0, ShuffleMode::kFalse);
  EXPECT_NE(ds, nullptr);

  // Create an iterator over the result of the above dataset
  // This will trigger the creation of the Execution Tree and launch it.
  std::shared_ptr<Iterator> iter = ds->CreateIterator();
  EXPECT_NE(iter, nullptr);

  // Iterate the dataset and get each row
  std::unordered_map<std::string, std::shared_ptr<Tensor>> row;
  iter->GetNextRow(&row);
  EXPECT_NE(row.find("col1"), row.end());
  std::vector<std::vector<std::string>> expected_result = {
    {"1", "2", "3", "4"},
    {"5", "6", "7", "8"},
    {"9", "10", "11", "12"},
  };

  uint64_t i = 0;
  while (row.size() != 0) {
    for (int j = 0; j < column_names.size(); j++) {
      auto text = row[column_names[j]];
      std::string_view sv;
      text->GetItemAt(&sv, {0});
      std::string ss(sv);
      EXPECT_STREQ(ss.c_str(), expected_result[i][j].c_str());
    }
    iter->GetNextRow(&row);
    i++;
  }

  // Expect 3 samples
  EXPECT_EQ(i, 3);

  // Manually terminate the pipeline
  iter->Stop();
}

TEST_F(MindDataTestPipeline, TestCSVDatasetMultiFiles) {
  MS_LOG(INFO) << "Doing MindDataTestPipeline-TestCSVDatasetMultiFiles.";

  // Set configuration
  uint32_t original_seed = GlobalContext::config_manager()->seed();
  uint32_t original_num_parallel_workers = GlobalContext::config_manager()->num_parallel_workers();
  MS_LOG(DEBUG) << "ORIGINAL seed: " << original_seed << ", num_parallel_workers: " << original_num_parallel_workers;
  GlobalContext::config_manager()->set_seed(111);
  GlobalContext::config_manager()->set_num_parallel_workers(4);

  // Create a CSVDataset, with single CSV file
  std::string file1 = datasets_root_path_ + "/testCSV/1.csv";
  std::string file2 = datasets_root_path_ + "/testCSV/append.csv";
  std::vector<std::string> column_names = {"col1", "col2", "col3", "col4"};
  std::shared_ptr<Dataset> ds = CSV({file1, file2}, ',', {}, column_names, 0, ShuffleMode::kGlobal);
  EXPECT_NE(ds, nullptr);

  // Create an iterator over the result of the above dataset
  // This will trigger the creation of the Execution Tree and launch it.
  std::shared_ptr<Iterator> iter = ds->CreateIterator();
  EXPECT_NE(iter, nullptr);

  // Iterate the dataset and get each row
  std::unordered_map<std::string, std::shared_ptr<Tensor>> row;
  iter->GetNextRow(&row);
  EXPECT_NE(row.find("col1"), row.end());
  std::vector<std::vector<std::string>> expected_result = {
    {"17", "18", "19", "20"}, {"1", "2", "3", "4"},     {"5", "6", "7", "8"},
    {"13", "14", "15", "16"}, {"21", "22", "23", "24"}, {"9", "10", "11", "12"},
  };

  uint64_t i = 0;
  while (row.size() != 0) {
    for (int j = 0; j < column_names.size(); j++) {
      auto text = row[column_names[j]];
      std::string_view sv;
      text->GetItemAt(&sv, {0});
      std::string ss(sv);
      EXPECT_STREQ(ss.c_str(), expected_result[i][j].c_str());
    }
    iter->GetNextRow(&row);
    i++;
  }

  // Expect 6 samples
  EXPECT_EQ(i, 6);

  // Manually terminate the pipeline
  iter->Stop();

  // Restore configuration
  GlobalContext::config_manager()->set_seed(original_seed);
  GlobalContext::config_manager()->set_num_parallel_workers(original_num_parallel_workers);
}

TEST_F(MindDataTestPipeline, TestCSVDatasetNumSamples) {
  MS_LOG(INFO) << "Doing MindDataTestPipeline-TestCSVDatasetNumSamples.";

  // Create a CSVDataset, with single CSV file
  std::string file = datasets_root_path_ + "/testCSV/1.csv";
  std::vector<std::string> column_names = {"col1", "col2", "col3", "col4"};
  std::shared_ptr<Dataset> ds = CSV({file}, ',', {}, column_names, 2, ShuffleMode::kFalse);
  EXPECT_NE(ds, nullptr);

  // Create an iterator over the result of the above dataset
  // This will trigger the creation of the Execution Tree and launch it.
  std::shared_ptr<Iterator> iter = ds->CreateIterator();
  EXPECT_NE(iter, nullptr);

  // Iterate the dataset and get each row
  std::unordered_map<std::string, std::shared_ptr<Tensor>> row;
  iter->GetNextRow(&row);
  EXPECT_NE(row.find("col1"), row.end());
  std::vector<std::vector<std::string>> expected_result = {{"1", "2", "3", "4"}, {"5", "6", "7", "8"}};

  uint64_t i = 0;
  while (row.size() != 0) {
    for (int j = 0; j < column_names.size(); j++) {
      auto text = row[column_names[j]];
      std::string_view sv;
      text->GetItemAt(&sv, {0});
      std::string ss(sv);
      EXPECT_STREQ(ss.c_str(), expected_result[i][j].c_str());
    }
    iter->GetNextRow(&row);
    i++;
  }

  // Expect 2 samples
  EXPECT_EQ(i, 2);

  // Manually terminate the pipeline
  iter->Stop();
}

TEST_F(MindDataTestPipeline, TestCSVDatasetDistribution) {
  MS_LOG(INFO) << "Doing MindDataTestPipeline-TestCSVDatasetDistribution.";

  // Create a CSVDataset, with single CSV file
  std::string file = datasets_root_path_ + "/testCSV/1.csv";
  std::vector<std::string> column_names = {"col1", "col2", "col3", "col4"};
  std::shared_ptr<Dataset> ds = CSV({file}, ',', {}, column_names, 0, ShuffleMode::kFalse, 2, 0);
  EXPECT_NE(ds, nullptr);

  // Create an iterator over the result of the above dataset
  // This will trigger the creation of the Execution Tree and launch it.
  std::shared_ptr<Iterator> iter = ds->CreateIterator();
  EXPECT_NE(iter, nullptr);

  // Iterate the dataset and get each row
  std::unordered_map<std::string, std::shared_ptr<Tensor>> row;
  iter->GetNextRow(&row);
  EXPECT_NE(row.find("col1"), row.end());
  std::vector<std::vector<std::string>> expected_result = {{"1", "2", "3", "4"}, {"5", "6", "7", "8"}};

  uint64_t i = 0;
  while (row.size() != 0) {
    for (int j = 0; j < column_names.size(); j++) {
      auto text = row[column_names[j]];
      std::string_view sv;
      text->GetItemAt(&sv, {0});
      std::string ss(sv);
      EXPECT_STREQ(ss.c_str(), expected_result[i][j].c_str());
    }
    iter->GetNextRow(&row);
    i++;
  }

  // Expect 2 samples
  EXPECT_EQ(i, 2);

  // Manually terminate the pipeline
  iter->Stop();
}

TEST_F(MindDataTestPipeline, TestCSVDatasetType) {
  MS_LOG(INFO) << "Doing MindDataTestPipeline-TestCSVDatasetType.";

  // Create a CSVDataset, with single CSV file
  std::string file = datasets_root_path_ + "/testCSV/default.csv";
  std::vector<std::shared_ptr<CsvBase>> colum_type = {
    std::make_shared<CsvRecord<std::string>>(CsvType::STRING, ""),
    std::make_shared<CsvRecord<int>>(CsvType::INT, 0),
    std::make_shared<CsvRecord<float>>(CsvType::FLOAT, 0.0),
    std::make_shared<CsvRecord<std::string>>(CsvType::STRING, ""),
  };
  std::vector<std::string> column_names = {"col1", "col2", "col3", "col4"};
  std::shared_ptr<Dataset> ds = CSV({file}, ',', colum_type, column_names, 0, ShuffleMode::kFalse);
  EXPECT_NE(ds, nullptr);

  // Create an iterator over the result of the above dataset
  // This will trigger the creation of the Execution Tree and launch it.
  std::shared_ptr<Iterator> iter = ds->CreateIterator();
  EXPECT_NE(iter, nullptr);

  // Iterate the dataset and get each row
  std::unordered_map<std::string, std::shared_ptr<Tensor>> row;
  iter->GetNextRow(&row);
  std::vector<std::vector<std::shared_ptr<CsvBase>>> expected = {
    {
      std::make_shared<CsvRecord<std::string>>(CsvType::STRING, ""),
      std::make_shared<CsvRecord<int>>(CsvType::INT, 2),
      std::make_shared<CsvRecord<float>>(CsvType::FLOAT, 3.0),
      std::make_shared<CsvRecord<std::string>>(CsvType::STRING, ""),
    },
    {
      std::make_shared<CsvRecord<std::string>>(CsvType::STRING, "a"),
      std::make_shared<CsvRecord<int>>(CsvType::INT, 4),
      std::make_shared<CsvRecord<float>>(CsvType::FLOAT, 5.0),
      std::make_shared<CsvRecord<std::string>>(CsvType::STRING, "b"),
    },
  };
  EXPECT_NE(row.find("col1"), row.end());

  uint64_t i = 0;
  while (row.size() != 0) {
    for (int j = 0; j < column_names.size(); j++) {
      auto text = row[column_names[j]];
      if (colum_type[j]->type == CsvType::INT) {
        int val;
        text->GetItemAt(&val, {0});
        EXPECT_EQ(val, std::dynamic_pointer_cast<CsvRecord<int>>(expected[i][j])->value);
      } else if (colum_type[j]->type == CsvType::FLOAT) {
        float val;
        text->GetItemAt(&val, {0});
        EXPECT_EQ(val, std::dynamic_pointer_cast<CsvRecord<float>>(expected[i][j])->value);
      } else if (colum_type[j]->type == CsvType::STRING) {
        std::string_view sv;
        text->GetItemAt(&sv, {0});
        std::string ss(sv);
        EXPECT_STREQ(ss.c_str(), std::dynamic_pointer_cast<CsvRecord<std::string>>(expected[i][j])->value.c_str());
      }
    }
    iter->GetNextRow(&row);
    i++;
  }

  // Expect 2 samples
  EXPECT_EQ(i, 2);

  // Manually terminate the pipeline
  iter->Stop();
}

TEST_F(MindDataTestPipeline, TestCSVDatasetHeader) {
  MS_LOG(INFO) << "Doing MindDataTestPipeline-TestCSVDatasetHeader.";

  // Create a CSVDataset, with single CSV file
  std::string train_file = datasets_root_path_ + "/testCSV/header.csv";
  std::shared_ptr<Dataset> ds = CSV({train_file}, ',', {}, {});
  EXPECT_NE(ds, nullptr);

  // Create an iterator over the result of the above dataset
  // This will trigger the creation of the Execution Tree and launch it.
  std::shared_ptr<Iterator> iter = ds->CreateIterator();
  EXPECT_NE(iter, nullptr);

  // Iterate the dataset and get each row
  std::unordered_map<std::string, std::shared_ptr<Tensor>> row;
  iter->GetNextRow(&row);
  EXPECT_NE(row.find("col1"), row.end());
  std::vector<std::vector<std::string>> expected_result = {
    {"a", "b", "c", "d"},
  };

  uint64_t i = 0;
  std::vector<std::string> column_names = {"col1", "col2", "col3", "col4"};
  while (row.size() != 0) {
    for (int j = 0; j < column_names.size(); j++) {
      auto text = row[column_names[j]];
      std::string_view sv;
      text->GetItemAt(&sv, {0});
      std::string ss(sv);
      EXPECT_STREQ(ss.c_str(), expected_result[i][j].c_str());
    }
    iter->GetNextRow(&row);
    i++;
  }

  // Expect 3 samples
  EXPECT_EQ(i, 1);

  // Manually terminate the pipeline
  iter->Stop();
}

TEST_F(MindDataTestPipeline, TestCSVDatasetException) {
  MS_LOG(INFO) << "Doing MindDataTestPipeline-TestCSVDatasetException.";
  // Create a CSV Dataset
  std::string file = datasets_root_path_ + "/testCSV/1.csv";
  std::string invalid_csv_file = "./NotExistFile";
  std::vector<std::string> column_names = {"col1", "col2", "col3", "col4"};

  // Test empty file list
  std::shared_ptr<Dataset> ds0 = CSV({});
  EXPECT_EQ(ds0, nullptr);

  // Test invalid file
  std::shared_ptr<Dataset> ds1 = CSV({invalid_csv_file});
  EXPECT_EQ(ds1, nullptr);

  // Test invalid num_samples < -1
  std::shared_ptr<Dataset> ds2 = CSV({file}, ',', {}, column_names, -1);
  EXPECT_EQ(ds2, nullptr);

  // Test invalid num_shards < 1
  std::shared_ptr<Dataset> ds3 = CSV({file}, ',', {}, column_names, 0, ShuffleMode::kFalse, 0);
  EXPECT_EQ(ds3, nullptr);

  // Test invalid shard_id >= num_shards
  std::shared_ptr<Dataset> ds4 = CSV({file}, ',', {}, column_names, 0, ShuffleMode::kFalse, 2, 2);
  EXPECT_EQ(ds4, nullptr);

  // Test invalid field_delim
  std::shared_ptr<Dataset> ds5 = CSV({file}, '"', {}, column_names);
  EXPECT_EQ(ds5, nullptr);
}

TEST_F(MindDataTestPipeline, TestCSVDatasetShuffleFilesA) {
  MS_LOG(INFO) << "Doing MindDataTestPipeline-TestCSVDatasetShuffleFilesA.";

  // Set configuration
  uint32_t original_seed = GlobalContext::config_manager()->seed();
  uint32_t original_num_parallel_workers = GlobalContext::config_manager()->num_parallel_workers();
  MS_LOG(DEBUG) << "ORIGINAL seed: " << original_seed << ", num_parallel_workers: " << original_num_parallel_workers;
  GlobalContext::config_manager()->set_seed(130);
  GlobalContext::config_manager()->set_num_parallel_workers(4);

  // Create a CSVDataset, with 2 CSV files, 1.csv and append.csv in lexicographical order
  std::string file1 = datasets_root_path_ + "/testCSV/1.csv";
  std::string file2 = datasets_root_path_ + "/testCSV/append.csv";
  std::vector<std::string> column_names = {"col1", "col2", "col3", "col4"};
  std::shared_ptr<Dataset> ds = CSV({file1, file2}, ',', {}, column_names, 0, ShuffleMode::kFiles);
  EXPECT_NE(ds, nullptr);

  // Create an iterator over the result of the above dataset
  // This will trigger the creation of the Execution Tree and launch it.
  std::shared_ptr<Iterator> iter = ds->CreateIterator();
  EXPECT_NE(iter, nullptr);

  // Iterate the dataset and get each row
  std::unordered_map<std::string, std::shared_ptr<Tensor>> row;
  iter->GetNextRow(&row);
  EXPECT_NE(row.find("col1"), row.end());
  std::vector<std::vector<std::string>> expected_result = {
    {"13", "14", "15", "16"}, {"1", "2", "3", "4"},     {"17", "18", "19", "20"},
    {"5", "6", "7", "8"},     {"21", "22", "23", "24"}, {"9", "10", "11", "12"},
  };

  uint64_t i = 0;
  while (row.size() != 0) {
    for (int j = 0; j < column_names.size(); j++) {
      auto text = row[column_names[j]];
      std::string_view sv;
      text->GetItemAt(&sv, {0});
      std::string ss(sv);
      EXPECT_STREQ(ss.c_str(), expected_result[i][j].c_str());
    }
    iter->GetNextRow(&row);
    i++;
  }

  // Expect 6 samples
  EXPECT_EQ(i, 6);

  // Manually terminate the pipeline
  iter->Stop();

  // Restore configuration
  GlobalContext::config_manager()->set_seed(original_seed);
  GlobalContext::config_manager()->set_num_parallel_workers(original_num_parallel_workers);
}

TEST_F(MindDataTestPipeline, TestCSVDatasetShuffleFilesB) {
  MS_LOG(INFO) << "Doing MindDataTestPipeline-TestCSVDatasetShuffleFilesB.";

  // Set configuration
  uint32_t original_seed = GlobalContext::config_manager()->seed();
  uint32_t original_num_parallel_workers = GlobalContext::config_manager()->num_parallel_workers();
  MS_LOG(DEBUG) << "ORIGINAL seed: " << original_seed << ", num_parallel_workers: " << original_num_parallel_workers;
  GlobalContext::config_manager()->set_seed(130);
  GlobalContext::config_manager()->set_num_parallel_workers(4);

  // Create a CSVDataset, with 2 CSV files, append.csv and 1.csv in non-lexicographical order
  std::string file1 = datasets_root_path_ + "/testCSV/1.csv";
  std::string file2 = datasets_root_path_ + "/testCSV/append.csv";
  std::vector<std::string> column_names = {"col1", "col2", "col3", "col4"};
  std::shared_ptr<Dataset> ds = CSV({file2, file1}, ',', {}, column_names, 0, ShuffleMode::kFiles);
  EXPECT_NE(ds, nullptr);

  // Create an iterator over the result of the above dataset
  // This will trigger the creation of the Execution Tree and launch it.
  std::shared_ptr<Iterator> iter = ds->CreateIterator();
  EXPECT_NE(iter, nullptr);

  // Iterate the dataset and get each row
  std::unordered_map<std::string, std::shared_ptr<Tensor>> row;
  iter->GetNextRow(&row);
  EXPECT_NE(row.find("col1"), row.end());
  std::vector<std::vector<std::string>> expected_result = {
    {"13", "14", "15", "16"}, {"1", "2", "3", "4"},     {"17", "18", "19", "20"},
    {"5", "6", "7", "8"},     {"21", "22", "23", "24"}, {"9", "10", "11", "12"},
  };

  uint64_t i = 0;
  while (row.size() != 0) {
    for (int j = 0; j < column_names.size(); j++) {
      auto text = row[column_names[j]];
      std::string_view sv;
      text->GetItemAt(&sv, {0});
      std::string ss(sv);
      MS_LOG(INFO) << "Text length: " << ss.length() << ", Text: " << ss.substr(0, 50);
      EXPECT_STREQ(ss.c_str(), expected_result[i][j].c_str());
    }
    iter->GetNextRow(&row);
    i++;
  }

  // Expect 6 samples
  EXPECT_EQ(i, 6);

  // Manually terminate the pipeline
  iter->Stop();

  // Restore configuration
  GlobalContext::config_manager()->set_seed(original_seed);
  GlobalContext::config_manager()->set_num_parallel_workers(original_num_parallel_workers);
}

TEST_F(MindDataTestPipeline, TestCSVDatasetShuffleGlobal) {
  MS_LOG(INFO) << "Doing MindDataTestPipeline-TestCSVDatasetShuffleGlobal.";
  // Test CSV Dataset with GLOBLE shuffle

  // Set configuration
  uint32_t original_seed = GlobalContext::config_manager()->seed();
  uint32_t original_num_parallel_workers = GlobalContext::config_manager()->num_parallel_workers();
  MS_LOG(DEBUG) << "ORIGINAL seed: " << original_seed << ", num_parallel_workers: " << original_num_parallel_workers;
  GlobalContext::config_manager()->set_seed(135);
  GlobalContext::config_manager()->set_num_parallel_workers(4);

  // Create a CSVFile Dataset, with single CSV file
  std::string train_file = datasets_root_path_ + "/testCSV/1.csv";
  std::vector<std::string> column_names = {"col1", "col2", "col3", "col4"};
  std::shared_ptr<Dataset> ds = CSV({train_file}, ',', {}, column_names, 0, ShuffleMode::kGlobal);
  EXPECT_NE(ds, nullptr);

  // Create an iterator over the result of the above dataset
  // This will trigger the creation of the Execution Tree and launch it.
  std::shared_ptr<Iterator> iter = ds->CreateIterator();
  EXPECT_NE(iter, nullptr);

  // Iterate the dataset and get each row
  std::unordered_map<std::string, std::shared_ptr<Tensor>> row;
  iter->GetNextRow(&row);
  EXPECT_NE(row.find("col1"), row.end());
  std::vector<std::vector<std::string>> expected_result = {
    {"5", "6", "7", "8"}, {"9", "10", "11", "12"}, {"1", "2", "3", "4"}};

  uint64_t i = 0;
  while (row.size() != 0) {
    for (int j = 0; j < column_names.size(); j++) {
      auto text = row[column_names[j]];
      std::string_view sv;
      text->GetItemAt(&sv, {0});
      std::string ss(sv);
      EXPECT_STREQ(ss.c_str(), expected_result[i][j].c_str());
    }
    iter->GetNextRow(&row);
    i++;
  }

  // Expect 3 samples
  EXPECT_EQ(i, 3);

  // Manually terminate the pipeline
  iter->Stop();

  // Restore configuration
  GlobalContext::config_manager()->set_seed(original_seed);
  GlobalContext::config_manager()->set_num_parallel_workers(original_num_parallel_workers);
}

TEST_F(MindDataTestPipeline, TestCSVDatasetDuplicateColumnName) {
  MS_LOG(INFO) << "Doing MindDataTestPipeline-TestCSVDatasetDuplicateColumnName.";

  // Create a CSVDataset, with single CSV file
  std::string train_file = datasets_root_path_ + "/testCSV/1.csv";
  std::vector<std::string> column_names = {"col1", "col1", "col3", "col4"};
  std::shared_ptr<Dataset> ds = CSV({train_file}, ',', {}, column_names, 0, ShuffleMode::kFalse);
  // Expect failure: duplicate column names
  EXPECT_EQ(ds, nullptr);
}
