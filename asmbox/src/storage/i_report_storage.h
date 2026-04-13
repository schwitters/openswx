// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace sw_dumper::storage {

struct WorkspaceInfo {
  std::string name;
  std::string created_at;
};

class IReportStorage {
 public:
  virtual ~IReportStorage() = default;

  // Workspace Management
  virtual bool CreateWorkspace(const std::string& name) = 0;
  virtual std::vector<WorkspaceInfo> GetWorkspaces() = 0;
  virtual bool DeleteWorkspace(
      const std::string& name) = 0;  // Löscht DB-Einträge

  // Data handling
  virtual bool Save(const std::string& workspace_name,
                    const std::string& rel_path,
                    const nlohmann::json& data) = 0;
  virtual bool Load(const std::string& workspace_name,
                    const std::string& rel_path,
                    nlohmann::json* data_out) = 0;
};

}  // namespace sw_dumper::storage
