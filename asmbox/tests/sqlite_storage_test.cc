// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#include <gtest/gtest.h>

#include <filesystem>
#include <random>
#include <string>

#include "sqlite3.h"
#include "storage/sqlite_storage.h"

namespace fs = std::filesystem;

namespace sw_dumper::storage {
namespace {

class TempDirectory {
 public:
  TempDirectory() {
    std::random_device random_device;
    const auto nonce = std::to_string(random_device());
    path_ = fs::temp_directory_path() / ("openswx_sqlite_test_" + nonce);
    fs::create_directories(path_);
  }

  ~TempDirectory() { std::error_code ignored; fs::remove_all(path_, ignored); }

  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

bool HasIndex(sqlite3* db, const std::string& index_name) {
  sqlite3_stmt* stmt = nullptr;
  constexpr char kSql[] =
      "SELECT COUNT(*) FROM sqlite_master "
      "WHERE type = 'index' AND name = ?;";
  if (sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }

  sqlite3_bind_text(stmt, 1, index_name.c_str(), -1, SQLITE_TRANSIENT);
  const int step_result = sqlite3_step(stmt);
  const bool has_index =
      step_result == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1;
  sqlite3_finalize(stmt);
  return has_index;
}

TEST(SqliteStorageTest, InitializationCreatesDeferredIndexes) {
  TempDirectory temp_directory;
  const fs::path db_path = temp_directory.path() / "global.sqlite";

  SqliteStorage storage(db_path.string());
  (void)storage;

  sqlite3* db = nullptr;
  ASSERT_EQ(sqlite3_open(db_path.string().c_str(), &db), SQLITE_OK);

  EXPECT_TRUE(HasIndex(db, "idx_props_parent"));
  EXPECT_TRUE(HasIndex(db, "idx_bom_items_bom"));
  EXPECT_TRUE(HasIndex(db, "idx_bom_items_par"));
  EXPECT_TRUE(HasIndex(db, "idx_prof_map_pid"));
  EXPECT_TRUE(HasIndex(db, "idx_prof_rule_pid"));

  sqlite3_close(db);
}

}  // namespace
}  // namespace sw_dumper::storage
