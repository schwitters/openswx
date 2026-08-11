// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

struct sqlite3;

namespace sw_dumper::storage {

class SqliteStorage {
  sqlite3* db_ = nullptr;
  void Exec(const std::string& sql);
  void InsertProps(int64_t parent_id,
                   const std::string& parent_type,
                   const nlohmann::json& props);

 public:
  explicit SqliteStorage(const std::string& db_path);
  ~SqliteStorage();

  bool Save(const std::string& rel_path, const nlohmann::json& data);
  bool Load(const std::string& rel_path, nlohmann::json* data_out);

  std::vector<std::string> GetPropertyNames();
  nlohmann::json GetPropertyConfig();
  bool SetPropertyConfig(const nlohmann::json& config);

  std::string GetPartNumberProp();
  bool SaveBom(const std::string& rel_path,
               const std::string& configuration,
               const std::string& part_number_prop,
               const nlohmann::json& bom_json);
};

}  // namespace sw_dumper::storage
