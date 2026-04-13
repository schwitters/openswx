// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include "i_report_storage.h"

struct sqlite3;

namespace sw_dumper::storage {

class SqliteStorage : public IReportStorage {
  sqlite3* db_ = nullptr;
  void Exec(const std::string& sql);
  void InsertProps(int64_t parent_id,
                   const std::string& parent_type,
                   const nlohmann::json& props);

 public:
  SqliteStorage(const std::string& db_path);
  ~SqliteStorage();

  bool CreateWorkspace(const std::string& name) override;
  std::vector<WorkspaceInfo> GetWorkspaces() override;
  bool DeleteWorkspace(const std::string& name) override;

  bool Save(const std::string& session_id,
            const std::string& rel_path,
            const nlohmann::json& data) override;
  bool Load(const std::string& session_id,
            const std::string& rel_path,
            nlohmann::json* data_out) override;

  // Property index: distinct property names found across all analyzed docs.
  std::vector<std::string> GetPropertyNames(const std::string& workspace);

  // Property config: [{name, visible, role}] per workspace.
  // role values: "" | "part_number" | "description" | "material" | "revision"
  nlohmann::json GetPropertyConfig(const std::string& workspace);
  bool SetPropertyConfig(const std::string& workspace,
                         const nlohmann::json& config);

  // Returns the property name that has role "part_number" for the workspace,
  // or "" if none configured.
  std::string GetPartNumberProp(const std::string& workspace);

  // BOM persistence (Thrift-aligned schema, surrogate keys).
  // bom_json is the root BomItem node from JsonWriter output (bom_json["bom"]).
  // part_number_prop: name of the property to use as Part.number (may be "").
  bool SaveBom(const std::string& workspace,
               const std::string& rel_path,
               const std::string& configuration,
               const std::string& part_number_prop,
               const nlohmann::json& bom_json);

  // ── Profile management (workspace-independent) ────────────────────────────
  // A profile has: id, name, description, mappings:[{sw_property, target_entity,
  // target_field}], rules:[{rule_type, property_name, property_value}]
  //
  // target_entity: "part" | "bom" | "bom_item"
  // target_field (part):     number, name, name2, name3, name4, materialId,
  //                          drawingNumber, kind, weight, creator, editor,
  //                          replacedByNumber
  // target_field (bom_item): version, note1, note2, note3, extRef,
  //                          costCenter, refItemNo
  // target_field (bom):      version, note1, note2, changeIndex
  //
  // rule_type: "kaufgruppe" | "non_bom" | "phantom"
  nlohmann::json GetProfiles();
  int64_t CreateProfile(const std::string& name, const std::string& description);
  bool DeleteProfile(int64_t profile_id);
  nlohmann::json GetProfile(int64_t profile_id);  // full: name+desc+mappings+rules
  bool SaveProfile(const nlohmann::json& profile); // upsert by id
};

}  // namespace sw_dumper::storage
