// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#include "sqlite_storage.h"
#include <plog/Log.h>
#include <sqlite3.h>
#include <filesystem>
#include <iostream>
#include <vector>
#include "sqlite_helpers.h"

namespace sw_dumper::storage {

class SqlTransaction {
  sqlite3* db_;
  bool committed_ = false;

 public:
  SqlTransaction(sqlite3* db) : db_(db) {
    if (sqlite3_get_autocommit(db) == 0)
      sqlite3_exec(db, "ROLLBACK;", 0, 0, nullptr);
    sqlite3_exec(db, "BEGIN TRANSACTION;", 0, 0, nullptr);
  }
  ~SqlTransaction() {
    if (!committed_)
      sqlite3_exec(db_, "ROLLBACK;", 0, 0, nullptr);
  }
  void Commit() {
    if (sqlite3_exec(db_, "COMMIT;", 0, 0, nullptr) == SQLITE_OK)
      committed_ = true;
  }
};

void SqliteStorage::Exec(const std::string& sql) {
  char* err = nullptr;
  if (sqlite3_exec(db_, sql.c_str(), 0, 0, &err) != SQLITE_OK) {
    PLOG_ERROR << "SQL Error: " << (err ? err : "")
               << " in: " << sql.substr(0, 30);
    if (err)
      sqlite3_free(err);
  }
}

void SqliteStorage::InsertProps(int64_t parent_id,
                                const std::string& parent_type,
                                const nlohmann::json& props) {
  if (!props.is_object())
    return;
  sqlite3_stmt* stmt;
  const char* sql =
      "INSERT INTO properties (parent_type, parent_id, name, value) VALUES (?, "
      "?, ?, ?);";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK)
    return;
  for (auto& [key, val] : props.items()) {
    std::string v_str;
    if (val.is_string())
      v_str = val.get<std::string>();
    else if (val.is_number())
      v_str = std::to_string(val.get<double>());
    else if (!val.is_null())
      v_str = val.dump();
    else
      continue;

    // Hier ist SQLITE_TRANSIENT wichtig, da v_str lokal ist!
    sqlite3_bind_text(stmt, 1, parent_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, parent_id);
    sqlite3_bind_text(stmt, 3, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, v_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_reset(stmt);
  }
  sqlite3_finalize(stmt);
}

nlohmann::json LoadPropsHelper(sqlite3* db,
                               int64_t parent_id,
                               const std::string& parent_type) {
  nlohmann::json j = nlohmann::json::object();
  sqlite3_stmt* stmt;
  const char* sql =
      "SELECT name, value FROM properties WHERE parent_type=? AND parent_id=?;";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) != SQLITE_OK)
    return j;
  sqlite3_bind_text(stmt, 1, parent_type.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 2, parent_id);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    j[SafeColumnText(stmt, 0)] = SafeColumnText(stmt, 1);
  }
  sqlite3_finalize(stmt);
  return j;
}

SqliteStorage::SqliteStorage(const std::string& db_path) {
  try {
    std::filesystem::create_directories(
        std::filesystem::path(db_path).parent_path());
  } catch (...) {
  }
  if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
    PLOG_FATAL << "DB Open Error";
    db_ = nullptr;
    return;
  }
  Exec(
      "PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL; PRAGMA "
      "foreign_keys = ON;");
  const char* schema = R"(
        CREATE TABLE IF NOT EXISTS workspaces (name TEXT PRIMARY KEY, created_at DATETIME DEFAULT CURRENT_TIMESTAMP);
        CREATE TABLE IF NOT EXISTS documents (id INTEGER PRIMARY KEY AUTOINCREMENT, session_id TEXT, rel_path TEXT, file_name TEXT, doc_type INTEGER, sw_version INTEGER, preview_image TEXT, created_at DATETIME DEFAULT CURRENT_TIMESTAMP, UNIQUE(session_id, rel_path), FOREIGN KEY(session_id) REFERENCES workspaces(name) ON DELETE CASCADE);
        CREATE TABLE IF NOT EXISTS configurations (id INTEGER PRIMARY KEY AUTOINCREMENT, document_id INTEGER, name TEXT, mass REAL, volume REAL, surface_area REAL, cog_x REAL, cog_y REAL, cog_z REAL, preview_image TEXT, FOREIGN KEY(document_id) REFERENCES documents(id) ON DELETE CASCADE);
        CREATE TABLE IF NOT EXISTS components (id INTEGER PRIMARY KEY AUTOINCREMENT, configuration_id INTEGER, name TEXT, ref_config TEXT, path TEXT, is_suppressed BOOLEAN, is_hidden BOOLEAN, exclude_from_bom BOOLEAN, component_ref TEXT, FOREIGN KEY(configuration_id) REFERENCES configurations(id) ON DELETE CASCADE);
        CREATE TABLE IF NOT EXISTS cut_lists (id INTEGER PRIMARY KEY AUTOINCREMENT, configuration_id INTEGER, name TEXT, quantity INTEGER, FOREIGN KEY(configuration_id) REFERENCES configurations(id) ON DELETE CASCADE);
        CREATE TABLE IF NOT EXISTS sheets (id INTEGER PRIMARY KEY AUTOINCREMENT, document_id INTEGER, name TEXT, preview_image TEXT, FOREIGN KEY(document_id) REFERENCES documents(id) ON DELETE CASCADE);
        CREATE TABLE IF NOT EXISTS views (id INTEGER PRIMARY KEY AUTOINCREMENT, sheet_id INTEGER, name TEXT, ref_doc TEXT, ref_config TEXT, FOREIGN KEY(sheet_id) REFERENCES sheets(id) ON DELETE CASCADE);
        CREATE TABLE IF NOT EXISTS properties (id INTEGER PRIMARY KEY AUTOINCREMENT, parent_type TEXT, parent_id INTEGER, name TEXT, value TEXT);
        CREATE TRIGGER IF NOT EXISTS del_doc_props BEFORE DELETE ON documents BEGIN DELETE FROM properties WHERE parent_type='DOC' AND parent_id=OLD.id; END;
        CREATE TRIGGER IF NOT EXISTS del_cfg_props BEFORE DELETE ON configurations BEGIN DELETE FROM properties WHERE parent_type='CONFIG' AND parent_id=OLD.id; END;
        CREATE TRIGGER IF NOT EXISTS del_cmp_props BEFORE DELETE ON components BEGIN DELETE FROM properties WHERE parent_type='COMP' AND parent_id=OLD.id; END;
        CREATE TRIGGER IF NOT EXISTS del_cut_props BEFORE DELETE ON cut_lists BEGIN DELETE FROM properties WHERE parent_type='CUTLIST' AND parent_id=OLD.id; END;
        CREATE TABLE IF NOT EXISTS property_configs (workspace TEXT NOT NULL, name TEXT NOT NULL, visible INTEGER NOT NULL DEFAULT 1, role TEXT NOT NULL DEFAULT '', PRIMARY KEY (workspace, name), FOREIGN KEY (workspace) REFERENCES workspaces(name) ON DELETE CASCADE);
        CREATE INDEX IF NOT EXISTS idx_props_parent    ON properties(parent_type, parent_id);
        CREATE INDEX IF NOT EXISTS idx_bom_items_bom   ON bom_items(bom_id);
        CREATE INDEX IF NOT EXISTS idx_bom_items_par   ON bom_items(parent_item_id);
        CREATE INDEX IF NOT EXISTS idx_prof_map_pid    ON profile_mappings(profile_id);
        CREATE INDEX IF NOT EXISTS idx_prof_rule_pid   ON profile_rules(profile_id);
        CREATE TABLE IF NOT EXISTS boms (
          id          INTEGER PRIMARY KEY AUTOINCREMENT,
          workspace   TEXT    NOT NULL,
          rel_path    TEXT    NOT NULL,
          configuration TEXT  NOT NULL DEFAULT '',
          part_number TEXT,
          part_name   TEXT,
          scope_id    INTEGER NOT NULL DEFAULT 1000005,
          type        INTEGER NOT NULL DEFAULT 10,
          version     TEXT,
          change_index TEXT,
          note1       TEXT,
          note2       TEXT,
          ext_ref     TEXT,
          created_at  DATETIME DEFAULT CURRENT_TIMESTAMP,
          UNIQUE(workspace, rel_path, configuration),
          FOREIGN KEY (workspace) REFERENCES workspaces(name) ON DELETE CASCADE
        );
        CREATE TABLE IF NOT EXISTS bom_items (
          id               INTEGER PRIMARY KEY AUTOINCREMENT,
          bom_id           INTEGER NOT NULL,
          parent_item_id   INTEGER,
          item_no          INTEGER,
          ref_item_no      TEXT,
          part_number      TEXT,
          part_name        TEXT,
          part_rel_path    TEXT,
          configuration    TEXT,
          quantity         REAL    NOT NULL DEFAULT 1,
          item_type        TEXT    NOT NULL DEFAULT 'part',
          version          TEXT,
          note1            TEXT,
          note2            TEXT,
          note3            TEXT,
          ext_ref          TEXT,
          is_suppressed    INTEGER NOT NULL DEFAULT 0,
          exclude_from_bom INTEGER NOT NULL DEFAULT 0,
          FOREIGN KEY (bom_id)         REFERENCES boms(id)      ON DELETE CASCADE,
          FOREIGN KEY (parent_item_id) REFERENCES bom_items(id) ON DELETE CASCADE
        );
        CREATE TABLE IF NOT EXISTS profiles (
          id          INTEGER PRIMARY KEY AUTOINCREMENT,
          name        TEXT    NOT NULL UNIQUE,
          description TEXT    NOT NULL DEFAULT '',
          created_at  DATETIME DEFAULT CURRENT_TIMESTAMP
        );
        CREATE TABLE IF NOT EXISTS profile_mappings (
          id            INTEGER PRIMARY KEY AUTOINCREMENT,
          profile_id    INTEGER NOT NULL,
          sw_property   TEXT    NOT NULL,
          target_entity TEXT    NOT NULL,
          target_field  TEXT    NOT NULL,
          UNIQUE(profile_id, target_entity, target_field),
          FOREIGN KEY (profile_id) REFERENCES profiles(id) ON DELETE CASCADE
        );
        CREATE TABLE IF NOT EXISTS profile_rules (
          id             INTEGER PRIMARY KEY AUTOINCREMENT,
          profile_id     INTEGER NOT NULL,
          rule_type      TEXT    NOT NULL,
          property_name  TEXT    NOT NULL,
          property_value TEXT    NOT NULL,
          FOREIGN KEY (profile_id) REFERENCES profiles(id) ON DELETE CASCADE
        );
    )";
  Exec(schema);
}

SqliteStorage::~SqliteStorage() {
  if (db_)
    sqlite3_close(db_);
}

bool SqliteStorage::CreateWorkspace(const std::string& name) {
  if (!db_)
    return false;
  sqlite3_stmt* stmt;
  const char* sql = "INSERT OR REPLACE INTO workspaces (name) VALUES (?);";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK)
    return false;
  sqlite3_bind_text(stmt, 1, name.c_str(), -1,
                    SQLITE_STATIC);  // name is arg, safe
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return (rc == SQLITE_DONE);
}

std::vector<WorkspaceInfo> SqliteStorage::GetWorkspaces() {
  std::vector<WorkspaceInfo> res;
  if (!db_)
    return res;
  sqlite3_stmt* stmt;
  if (sqlite3_prepare_v2(
          db_,
          "SELECT name, created_at FROM workspaces ORDER BY created_at DESC;",
          -1, &stmt, 0) != SQLITE_OK)
    return res;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    res.push_back({SafeColumnText(stmt, 0), SafeColumnText(stmt, 1)});
  }
  sqlite3_finalize(stmt);
  return res;
}

bool SqliteStorage::DeleteWorkspace(const std::string& name) {
  if (!db_)
    return false;
  sqlite3_stmt* stmt;
  if (sqlite3_prepare_v2(db_, "DELETE FROM workspaces WHERE name=?;", -1, &stmt,
                         0) != SQLITE_OK)
    return false;
  sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return (rc == SQLITE_DONE);
}

bool SqliteStorage::Save(const std::string& sess,
                         const std::string& path,
                         const nlohmann::json& d) {
  if (!db_)
    return false;
  SqlTransaction trans(db_);
  sqlite3_stmt* stmt;
  std::string sql_doc =
      "INSERT OR REPLACE INTO documents (session_id, rel_path, file_name, "
      "doc_type, sw_version, preview_image) VALUES (?,?,?,?,?,?);";
  if (sqlite3_prepare_v2(db_, sql_doc.c_str(), -1, &stmt, 0) != SQLITE_OK)
    return false;

  std::string fname = std::filesystem::path(path).filename().string();
  sqlite3_bind_text(stmt, 1, sess.c_str(), -1,
                    SQLITE_STATIC);  // sess lives long enough
  sqlite3_bind_text(stmt, 2, path.c_str(), -1,
                    SQLITE_STATIC);  // path lives long enough
  sqlite3_bind_text(stmt, 3, fname.c_str(), -1,
                    SQLITE_STATIC);  // fname local but lives until step
  sqlite3_bind_int(stmt, 4, d.value("doc_type", 0));
  sqlite3_bind_int(stmt, 5, d.value("version", 0));

  // FIX: value() returns temp string -> SQLITE_TRANSIENT
  std::string prev = d.value("preview_png_base64", "");
  sqlite3_bind_text(stmt, 6, prev.c_str(), -1, SQLITE_TRANSIENT);

  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    return false;
  }
  int64_t doc_id = sqlite3_last_insert_rowid(db_);
  sqlite3_finalize(stmt);
  if (d.contains("global_properties"))
    InsertProps(doc_id, "DOC", d["global_properties"]);

  // Config Loop
  if (d.contains("configurations") && d["configurations"].is_array()) {
    const char* sql_cfg =
        "INSERT INTO configurations (document_id, name, mass, volume, "
        "surface_area, cog_x, cog_y, cog_z, preview_image) VALUES "
        "(?,?,?,?,?,?,?,?,?);";
    sqlite3_stmt* stmt_cfg;
    sqlite3_prepare_v2(db_, sql_cfg, -1, &stmt_cfg, 0);
    for (const auto& c : d["configurations"]) {
      sqlite3_bind_int64(stmt_cfg, 1, doc_id);
      // FIX: SQLITE_TRANSIENT for all JSON string lookups
      sqlite3_bind_text(stmt_cfg, 2, c.value("name", "").c_str(), -1,
                        SQLITE_TRANSIENT);

      double mass = 0, vol = 0, area = 0, cx = 0, cy = 0, cz = 0;
      if (c.contains("mass_properties") && c["mass_properties"].is_object()) {
        const auto& m = c["mass_properties"];
        mass = m.value("mass", 0.0);
        vol = m.value("volume", 0.0);
        area = m.value("surface_area", 0.0);
        if (m.contains("center_of_gravity") &&
            m["center_of_gravity"].is_object()) {
          cx = m["center_of_gravity"].value("x", 0.0);
          cy = m["center_of_gravity"].value("y", 0.0);
          cz = m["center_of_gravity"].value("z", 0.0);
        }
      }
      sqlite3_bind_double(stmt_cfg, 3, mass);
      sqlite3_bind_double(stmt_cfg, 4, vol);
      sqlite3_bind_double(stmt_cfg, 5, area);
      sqlite3_bind_double(stmt_cfg, 6, cx);
      sqlite3_bind_double(stmt_cfg, 7, cy);
      sqlite3_bind_double(stmt_cfg, 8, cz);

      std::string c_prev = c.value("preview_png_base64", "");
      sqlite3_bind_text(stmt_cfg, 9, c_prev.c_str(), -1, SQLITE_TRANSIENT);

      sqlite3_step(stmt_cfg);
      int64_t cfg_id = sqlite3_last_insert_rowid(db_);
      sqlite3_reset(stmt_cfg);
      if (c.contains("properties"))
        InsertProps(cfg_id, "CONFIG", c["properties"]);

      if (c.contains("components") && c["components"].is_array()) {
        const char* sql_cmp =
            "INSERT INTO components (configuration_id, name, ref_config, path, "
            "is_suppressed, is_hidden, exclude_from_bom, component_ref) VALUES "
            "(?,?,?,?,?,?,?,?);";
        sqlite3_stmt* stmt_cmp;
        sqlite3_prepare_v2(db_, sql_cmp, -1, &stmt_cmp, 0);
        for (const auto& comp : c["components"]) {
          sqlite3_bind_int64(stmt_cmp, 1, cfg_id);
          // FIX: SQLITE_TRANSIENT everywhere!
          sqlite3_bind_text(stmt_cmp, 2, comp.value("name", "").c_str(), -1,
                            SQLITE_TRANSIENT);
          sqlite3_bind_text(stmt_cmp, 3, comp.value("ref_config", "").c_str(),
                            -1, SQLITE_TRANSIENT);
          sqlite3_bind_text(stmt_cmp, 4, comp.value("path", "").c_str(), -1,
                            SQLITE_TRANSIENT);
          sqlite3_bind_int(stmt_cmp, 5, comp.value("is_suppressed", false));
          sqlite3_bind_int(stmt_cmp, 6, comp.value("is_hidden", false));
          sqlite3_bind_int(stmt_cmp, 7, comp.value("exclude_from_bom", false));
          sqlite3_bind_text(stmt_cmp, 8,
                            comp.value("component_reference", "").c_str(), -1,
                            SQLITE_TRANSIENT);
          sqlite3_step(stmt_cmp);
          int64_t comp_id = sqlite3_last_insert_rowid(db_);
          sqlite3_reset(stmt_cmp);
          if (comp.contains("instance_properties"))
            InsertProps(comp_id, "COMP", comp["instance_properties"]);
        }
        sqlite3_finalize(stmt_cmp);
      }
      if (c.contains("cut_list") && c["cut_list"].is_array()) {
        const char* sql_cl =
            "INSERT INTO cut_lists (configuration_id, name, quantity) VALUES "
            "(?,?,?);";
        sqlite3_stmt* stmt_cl;
        sqlite3_prepare_v2(db_, sql_cl, -1, &stmt_cl, 0);
        for (const auto& item : c["cut_list"]) {
          sqlite3_bind_int64(stmt_cl, 1, cfg_id);
          sqlite3_bind_text(stmt_cl, 2, item.value("name", "").c_str(), -1,
                            SQLITE_TRANSIENT);
          sqlite3_bind_int(stmt_cl, 3, item.value("quantity", 1));
          sqlite3_step(stmt_cl);
          int64_t cl_id = sqlite3_last_insert_rowid(db_);
          sqlite3_reset(stmt_cl);
          if (item.contains("properties"))
            InsertProps(cl_id, "CUTLIST", item["properties"]);
        }
        sqlite3_finalize(stmt_cl);
      }
    }
    sqlite3_finalize(stmt_cfg);
  }
  // Sheets Loop
  if (d.contains("sheets") && d["sheets"].is_array()) {
    const char* sql_sht =
        "INSERT INTO sheets (document_id, name, preview_image) VALUES (?,?,?);";
    sqlite3_stmt* stmt_sht;
    sqlite3_prepare_v2(db_, sql_sht, -1, &stmt_sht, 0);
    const char* sql_vw =
        "INSERT INTO views (sheet_id, name, ref_doc, ref_config) VALUES "
        "(?,?,?,?);";
    sqlite3_stmt* stmt_vw;
    sqlite3_prepare_v2(db_, sql_vw, -1, &stmt_vw, 0);
    for (const auto& s : d["sheets"]) {
      sqlite3_bind_int64(stmt_sht, 1, doc_id);
      sqlite3_bind_text(stmt_sht, 2, s.value("name", "").c_str(), -1,
                        SQLITE_TRANSIENT);
      std::string s_prev = s.value("preview_png_base64", "");
      sqlite3_bind_text(stmt_sht, 3, s_prev.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_step(stmt_sht);
      int64_t sht_id = sqlite3_last_insert_rowid(db_);
      sqlite3_reset(stmt_sht);
      if (s.contains("views") && s["views"].is_array()) {
        for (const auto& v : s["views"]) {
          sqlite3_bind_int64(stmt_vw, 1, sht_id);
          sqlite3_bind_text(stmt_vw, 2, v.value("name", "").c_str(), -1,
                            SQLITE_TRANSIENT);
          sqlite3_bind_text(stmt_vw, 3,
                            v.value("referenced_document", "").c_str(), -1,
                            SQLITE_TRANSIENT);
          sqlite3_bind_text(stmt_vw, 4,
                            v.value("referenced_config", "").c_str(), -1,
                            SQLITE_TRANSIENT);
          sqlite3_step(stmt_vw);
          sqlite3_reset(stmt_vw);
        }
      }
    }
    sqlite3_finalize(stmt_sht);
    sqlite3_finalize(stmt_vw);
  }
  trans.Commit();
  return true;
}

// Load (Identisch zu 116, da hier keine transienten Strings erzeugt werden,
// SafeColumnText kopiert)
bool SqliteStorage::Load(const std::string& sess,
                         const std::string& path,
                         nlohmann::json* d) {
  if (!db_)
    return false;
  sqlite3_stmt* stmt;
  const char* sql =
      "SELECT id, doc_type, sw_version, preview_image FROM documents WHERE "
      "session_id=? AND rel_path=?;";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK)
    return false;
  sqlite3_bind_text(stmt, 1, sess.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, path.c_str(), -1, SQLITE_STATIC);
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return false;
  }
  int64_t doc_id = sqlite3_column_int64(stmt, 0);
  (*d)["doc_type"] = sqlite3_column_int(stmt, 1);
  (*d)["version"] = sqlite3_column_int(stmt, 2);
  (*d)["preview_png_base64"] = SafeColumnText(stmt, 3);
  nlohmann::json gp = LoadPropsHelper(db_, doc_id, "DOC");
  if (!gp.empty())
    (*d)["global_properties"] = gp;
  sqlite3_finalize(stmt);

  (*d)["configurations"] = nlohmann::json::array();
  const char* sql_cfg =
      "SELECT id, name, mass, volume, surface_area, cog_x, cog_y, cog_z, "
      "preview_image FROM configurations WHERE document_id=?;";
  if (sqlite3_prepare_v2(db_, sql_cfg, -1, &stmt, 0) == SQLITE_OK) {
    sqlite3_bind_int64(stmt, 1, doc_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      int64_t cfg_id = sqlite3_column_int64(stmt, 0);
      nlohmann::json c;
      c["name"] = SafeColumnText(stmt, 1);
      c["mass_properties"]["mass"] = sqlite3_column_double(stmt, 2);
      c["mass_properties"]["volume"] = sqlite3_column_double(stmt, 3);
      c["mass_properties"]["surface_area"] = sqlite3_column_double(stmt, 4);
      c["mass_properties"]["center_of_gravity"]["x"] =
          sqlite3_column_double(stmt, 5);
      c["mass_properties"]["center_of_gravity"]["y"] =
          sqlite3_column_double(stmt, 6);
      c["mass_properties"]["center_of_gravity"]["z"] =
          sqlite3_column_double(stmt, 7);
      c["preview_png_base64"] = SafeColumnText(stmt, 8);
      nlohmann::json cp = LoadPropsHelper(db_, cfg_id, "CONFIG");
      if (!cp.empty())
        c["properties"] = cp;

      c["components"] = nlohmann::json::array();
      const char* sql_cmp =
          "SELECT id, name, ref_config, path, is_suppressed, is_hidden, "
          "exclude_from_bom, component_ref FROM components WHERE "
          "configuration_id=?;";
      sqlite3_stmt* stmt_cmp;
      if (sqlite3_prepare_v2(db_, sql_cmp, -1, &stmt_cmp, 0) == SQLITE_OK) {
        sqlite3_bind_int64(stmt_cmp, 1, cfg_id);
        while (sqlite3_step(stmt_cmp) == SQLITE_ROW) {
          nlohmann::json comp;
          int64_t comp_id = sqlite3_column_int64(stmt_cmp, 0);
          comp["name"] = SafeColumnText(stmt_cmp, 1);
          comp["ref_config"] = SafeColumnText(stmt_cmp, 2);
          comp["path"] = SafeColumnText(stmt_cmp, 3);
          comp["is_suppressed"] = (bool)sqlite3_column_int(stmt_cmp, 4);
          comp["is_hidden"] = (bool)sqlite3_column_int(stmt_cmp, 5);
          comp["exclude_from_bom"] = (bool)sqlite3_column_int(stmt_cmp, 6);
          comp["component_reference"] = SafeColumnText(stmt_cmp, 7);
          nlohmann::json ip = LoadPropsHelper(db_, comp_id, "COMP");
          if (!ip.empty())
            comp["instance_properties"] = ip;
          c["components"].push_back(comp);
        }
        sqlite3_finalize(stmt_cmp);
      }

      c["cut_list"] = nlohmann::json::array();
      const char* sql_cl =
          "SELECT id, name, quantity FROM cut_lists WHERE configuration_id=?;";
      sqlite3_stmt* stmt_cl;
      if (sqlite3_prepare_v2(db_, sql_cl, -1, &stmt_cl, 0) == SQLITE_OK) {
        sqlite3_bind_int64(stmt_cl, 1, cfg_id);
        while (sqlite3_step(stmt_cl) == SQLITE_ROW) {
          nlohmann::json item;
          int64_t cl_id = sqlite3_column_int64(stmt_cl, 0);
          item["name"] = SafeColumnText(stmt_cl, 1);
          item["quantity"] = sqlite3_column_int(stmt_cl, 2);
          nlohmann::json clp = LoadPropsHelper(db_, cl_id, "CUTLIST");
          if (!clp.empty())
            item["properties"] = clp;
          c["cut_list"].push_back(item);
        }
        sqlite3_finalize(stmt_cl);
      }
      (*d)["configurations"].push_back(c);
    }
    sqlite3_finalize(stmt);
  }
  (*d)["sheets"] = nlohmann::json::array();
  const char* sql_sht =
      "SELECT id, name, preview_image FROM sheets WHERE document_id=?;";
  if (sqlite3_prepare_v2(db_, sql_sht, -1, &stmt, 0) == SQLITE_OK) {
    sqlite3_bind_int64(stmt, 1, doc_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      int64_t sht_id = sqlite3_column_int64(stmt, 0);
      nlohmann::json s;
      s["name"] = SafeColumnText(stmt, 1);
      s["preview_png_base64"] = SafeColumnText(stmt, 2);
      s["views"] = nlohmann::json::array();
      const char* sql_vw =
          "SELECT name, ref_doc, ref_config FROM views WHERE sheet_id=?;";
      sqlite3_stmt* stmt_vw;
      if (sqlite3_prepare_v2(db_, sql_vw, -1, &stmt_vw, 0) == SQLITE_OK) {
        sqlite3_bind_int64(stmt_vw, 1, sht_id);
        while (sqlite3_step(stmt_vw) == SQLITE_ROW) {
          nlohmann::json v;
          v["name"] = SafeColumnText(stmt_vw, 0);
          v["referenced_document"] = SafeColumnText(stmt_vw, 1);
          v["referenced_config"] = SafeColumnText(stmt_vw, 2);
          s["views"].push_back(v);
        }
        sqlite3_finalize(stmt_vw);
      }
      (*d)["sheets"].push_back(s);
    }
    sqlite3_finalize(stmt);
  }
  return true;
}

std::string SqliteStorage::GetPartNumberProp(const std::string& workspace) {
  if (!db_) return "";
  sqlite3_stmt* stmt;
  if (sqlite3_prepare_v2(db_,
        "SELECT name FROM property_configs WHERE workspace=? AND role='part_number' LIMIT 1;",
        -1, &stmt, 0) != SQLITE_OK) return "";
  sqlite3_bind_text(stmt, 1, workspace.c_str(), -1, SQLITE_STATIC);
  std::string result;
  if (sqlite3_step(stmt) == SQLITE_ROW) result = SafeColumnText(stmt, 0);
  sqlite3_finalize(stmt);
  return result;
}

bool SqliteStorage::SaveBom(const std::string& workspace,
                            const std::string& rel_path,
                            const std::string& configuration,
                            const std::string& part_number_prop,
                            const nlohmann::json& root_item) {
  if (!db_) return false;

  // Helper: extract part number from a BomItem JSON node.
  auto get_pn = [&](const nlohmann::json& item) -> std::string {
    if (!part_number_prop.empty() && item.contains("properties")) {
      const auto& props = item["properties"];
      if (props.contains(part_number_prop) && props[part_number_prop].is_string())
        return props[part_number_prop].get<std::string>();
    }
    // Fallback: use stem of file_path, then name.
    std::string fp = item.value("file_path", "");
    if (!fp.empty()) return std::filesystem::path(fp).stem().string();
    return item.value("name", "");
  };

  auto get_name = [&](const nlohmann::json& item) -> std::string {
    return item.value("name", "");
  };

  SqlTransaction tx(db_);

  // Delete existing BOM for this workspace/rel_path/configuration, then re-insert.
  {
    sqlite3_stmt* del;
    if (sqlite3_prepare_v2(db_,
          "DELETE FROM boms WHERE workspace=? AND rel_path=? AND configuration=?;",
          -1, &del, 0) == SQLITE_OK) {
      sqlite3_bind_text(del, 1, workspace.c_str(),     -1, SQLITE_STATIC);
      sqlite3_bind_text(del, 2, rel_path.c_str(),      -1, SQLITE_STATIC);
      sqlite3_bind_text(del, 3, configuration.c_str(), -1, SQLITE_STATIC);
      sqlite3_step(del);
      sqlite3_finalize(del);
    }
  }

  // Insert bom header.
  sqlite3_stmt* ins_bom;
  if (sqlite3_prepare_v2(db_,
        "INSERT INTO boms (workspace, rel_path, configuration, part_number, part_name) "
        "VALUES (?,?,?,?,?);",
        -1, &ins_bom, 0) != SQLITE_OK) return false;

  std::string root_pn   = get_pn(root_item);
  std::string root_name = get_name(root_item);
  sqlite3_bind_text(ins_bom, 1, workspace.c_str(),     -1, SQLITE_STATIC);
  sqlite3_bind_text(ins_bom, 2, rel_path.c_str(),      -1, SQLITE_STATIC);
  sqlite3_bind_text(ins_bom, 3, configuration.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(ins_bom, 4, root_pn.c_str(),   -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(ins_bom, 5, root_name.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(ins_bom) != SQLITE_DONE) {
    sqlite3_finalize(ins_bom);
    return false;
  }
  int64_t bom_id = sqlite3_last_insert_rowid(db_);
  sqlite3_finalize(ins_bom);

  // Recursively insert bom_items.
  sqlite3_stmt* ins_item;
  if (sqlite3_prepare_v2(db_,
        "INSERT INTO bom_items "
        "(bom_id, parent_item_id, item_no, part_number, part_name, "
        " part_rel_path, configuration, quantity, item_type, "
        " is_suppressed, exclude_from_bom) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?);",
        -1, &ins_item, 0) != SQLITE_OK) return false;

  // Iterative DFS using an explicit stack to avoid deep recursion.
  // Stack entry: {json item node, parent_item_id, item_no within parent}
  struct Frame { const nlohmann::json* node; int64_t parent_id; int seq; };
  std::vector<Frame> stack;
  const auto& children = root_item.value("children", nlohmann::json::array());
  for (int i = 0; i < static_cast<int>(children.size()); ++i)
    stack.push_back({&children[i], -1, i + 1});

  while (!stack.empty()) {
    auto [node, parent_id, seq] = stack.back();
    stack.pop_back();

    std::string pn   = get_pn(*node);
    std::string pname = get_name(*node);
    std::string fp    = node->value("file_path", "");
    std::string cfg   = node->value("configuration", "");
    std::string type  = node->value("type", "part");
    double qty        = node->value("quantity", 1.0);
    bool suppressed   = node->value("is_suppressed", false);
    bool excl         = node->value("exclude_from_bom", false);

    sqlite3_bind_int64(ins_item, 1, bom_id);
    if (parent_id < 0) sqlite3_bind_null(ins_item, 2);
    else               sqlite3_bind_int64(ins_item, 2, parent_id);
    sqlite3_bind_int  (ins_item, 3, seq);
    sqlite3_bind_text (ins_item, 4, pn.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (ins_item, 5, pname.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (ins_item, 6, fp.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (ins_item, 7, cfg.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(ins_item, 8, qty);
    sqlite3_bind_text (ins_item, 9, type.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_int  (ins_item, 10, suppressed ? 1 : 0);
    sqlite3_bind_int  (ins_item, 11, excl       ? 1 : 0);
    sqlite3_step(ins_item);
    int64_t item_id = sqlite3_last_insert_rowid(db_);
    sqlite3_reset(ins_item);

    // Push children in reverse so they're processed in order.
    const auto& ch = node->value("children", nlohmann::json::array());
    for (int i = static_cast<int>(ch.size()) - 1; i >= 0; --i)
      stack.push_back({&ch[i], item_id, i + 1});
  }

  sqlite3_finalize(ins_item);
  tx.Commit();
  return true;
}

std::vector<std::string> SqliteStorage::GetPropertyNames(
    const std::string& workspace) {
  std::vector<std::string> names;
  if (!db_) return names;
  // Collect distinct property names from global, config, and cut-list props.
  const char* sql =
      "SELECT DISTINCT p.name FROM properties p "
      "JOIN documents d ON p.parent_type='DOC' AND p.parent_id=d.id AND d.session_id=? "
      "UNION "
      "SELECT DISTINCT p.name FROM properties p "
      "JOIN configurations c ON p.parent_type='CONFIG' AND p.parent_id=c.id "
      "JOIN documents d ON c.document_id=d.id AND d.session_id=? "
      "UNION "
      "SELECT DISTINCT p.name FROM properties p "
      "JOIN cut_lists cl ON p.parent_type='CUTLIST' AND p.parent_id=cl.id "
      "JOIN configurations c ON cl.configuration_id=c.id "
      "JOIN documents d ON c.document_id=d.id AND d.session_id=? "
      "ORDER BY name;";
  sqlite3_stmt* stmt;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK) return names;
  sqlite3_bind_text(stmt, 1, workspace.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, workspace.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, workspace.c_str(), -1, SQLITE_STATIC);
  while (sqlite3_step(stmt) == SQLITE_ROW)
    names.push_back(SafeColumnText(stmt, 0));
  sqlite3_finalize(stmt);
  return names;
}

nlohmann::json SqliteStorage::GetPropertyConfig(const std::string& workspace) {
  nlohmann::json result = nlohmann::json::array();
  if (!db_) return result;
  sqlite3_stmt* stmt;
  const char* sql =
      "SELECT name, visible, role FROM property_configs WHERE workspace=? ORDER BY name;";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK) return result;
  sqlite3_bind_text(stmt, 1, workspace.c_str(), -1, SQLITE_STATIC);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    nlohmann::json item;
    item["name"]    = SafeColumnText(stmt, 0);
    item["visible"] = sqlite3_column_int(stmt, 1) != 0;
    item["role"]    = SafeColumnText(stmt, 2);
    result.push_back(item);
  }
  sqlite3_finalize(stmt);
  return result;
}

bool SqliteStorage::SetPropertyConfig(const std::string& workspace,
                                      const nlohmann::json& config) {
  if (!db_ || !config.is_array()) return false;
  SqlTransaction tx(db_);
  // Remove existing config for this workspace then re-insert.
  sqlite3_stmt* del_stmt;
  if (sqlite3_prepare_v2(db_,
        "DELETE FROM property_configs WHERE workspace=?;", -1, &del_stmt, 0) != SQLITE_OK)
    return false;
  sqlite3_bind_text(del_stmt, 1, workspace.c_str(), -1, SQLITE_STATIC);
  sqlite3_step(del_stmt);
  sqlite3_finalize(del_stmt);

  sqlite3_stmt* ins;
  if (sqlite3_prepare_v2(db_,
        "INSERT INTO property_configs (workspace, name, visible, role) VALUES (?,?,?,?);",
        -1, &ins, 0) != SQLITE_OK)
    return false;
  for (const auto& item : config) {
    sqlite3_bind_text(ins, 1, workspace.c_str(), -1, SQLITE_STATIC);
    std::string name = item.value("name", "");
    sqlite3_bind_text(ins, 2, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(ins, 3, item.value("visible", true) ? 1 : 0);
    std::string role = item.value("role", "");
    sqlite3_bind_text(ins, 4, role.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(ins);
    sqlite3_reset(ins);
  }
  sqlite3_finalize(ins);
  tx.Commit();
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Profile management
// ─────────────────────────────────────────────────────────────────────────────

nlohmann::json SqliteStorage::GetProfiles() {
  nlohmann::json result = nlohmann::json::array();
  if (!db_) return result;
  sqlite3_stmt* stmt;
  if (sqlite3_prepare_v2(db_,
        "SELECT id, name, description, created_at FROM profiles ORDER BY name;",
        -1, &stmt, 0) != SQLITE_OK) return result;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    nlohmann::json p;
    p["id"]          = sqlite3_column_int64(stmt, 0);
    p["name"]        = SafeColumnText(stmt, 1);
    p["description"] = SafeColumnText(stmt, 2);
    p["created_at"]  = SafeColumnText(stmt, 3);
    result.push_back(p);
  }
  sqlite3_finalize(stmt);
  return result;
}

int64_t SqliteStorage::CreateProfile(const std::string& name,
                                     const std::string& description) {
  if (!db_) return -1;
  sqlite3_stmt* stmt;
  if (sqlite3_prepare_v2(db_,
        "INSERT INTO profiles (name, description) VALUES (?, ?);",
        -1, &stmt, 0) != SQLITE_OK) return -1;
  sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, description.c_str(), -1, SQLITE_STATIC);
  if (sqlite3_step(stmt) != SQLITE_DONE) { sqlite3_finalize(stmt); return -1; }
  int64_t id = sqlite3_last_insert_rowid(db_);
  sqlite3_finalize(stmt);
  return id;
}

bool SqliteStorage::DeleteProfile(int64_t profile_id) {
  if (!db_) return false;
  sqlite3_stmt* stmt;
  if (sqlite3_prepare_v2(db_,
        "DELETE FROM profiles WHERE id=?;", -1, &stmt, 0) != SQLITE_OK) return false;
  sqlite3_bind_int64(stmt, 1, profile_id);
  bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
  return ok;
}

nlohmann::json SqliteStorage::GetProfile(int64_t profile_id) {
  nlohmann::json result;
  if (!db_) return result;

  // Header
  sqlite3_stmt* stmt;
  if (sqlite3_prepare_v2(db_,
        "SELECT id, name, description, created_at FROM profiles WHERE id=?;",
        -1, &stmt, 0) != SQLITE_OK) return result;
  sqlite3_bind_int64(stmt, 1, profile_id);
  if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); return result; }
  result["id"]          = sqlite3_column_int64(stmt, 0);
  result["name"]        = SafeColumnText(stmt, 1);
  result["description"] = SafeColumnText(stmt, 2);
  result["created_at"]  = SafeColumnText(stmt, 3);
  sqlite3_finalize(stmt);

  // Mappings
  result["mappings"] = nlohmann::json::array();
  if (sqlite3_prepare_v2(db_,
        "SELECT sw_property, target_entity, target_field "
        "FROM profile_mappings WHERE profile_id=? ORDER BY target_entity, target_field;",
        -1, &stmt, 0) == SQLITE_OK) {
    sqlite3_bind_int64(stmt, 1, profile_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      nlohmann::json m;
      m["sw_property"]   = SafeColumnText(stmt, 0);
      m["target_entity"] = SafeColumnText(stmt, 1);
      m["target_field"]  = SafeColumnText(stmt, 2);
      result["mappings"].push_back(m);
    }
    sqlite3_finalize(stmt);
  }

  // Rules
  result["rules"] = nlohmann::json::array();
  if (sqlite3_prepare_v2(db_,
        "SELECT rule_type, property_name, property_value "
        "FROM profile_rules WHERE profile_id=? ORDER BY rule_type, property_name;",
        -1, &stmt, 0) == SQLITE_OK) {
    sqlite3_bind_int64(stmt, 1, profile_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      nlohmann::json r;
      r["rule_type"]      = SafeColumnText(stmt, 0);
      r["property_name"]  = SafeColumnText(stmt, 1);
      r["property_value"] = SafeColumnText(stmt, 2);
      result["rules"].push_back(r);
    }
    sqlite3_finalize(stmt);
  }

  return result;
}

bool SqliteStorage::SaveProfile(const nlohmann::json& profile) {
  if (!db_ || !profile.is_object()) return false;
  int64_t profile_id = profile.value("id", int64_t(-1));
  if (profile_id < 0) return false;

  SqlTransaction tx(db_);

  // Update header
  sqlite3_stmt* stmt;
  if (sqlite3_prepare_v2(db_,
        "UPDATE profiles SET name=?, description=? WHERE id=?;",
        -1, &stmt, 0) != SQLITE_OK) return false;
  std::string name = profile.value("name", "");
  std::string desc = profile.value("description", "");
  sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, desc.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 3, profile_id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // Replace mappings
  if (sqlite3_prepare_v2(db_,
        "DELETE FROM profile_mappings WHERE profile_id=?;",
        -1, &stmt, 0) == SQLITE_OK) {
    sqlite3_bind_int64(stmt, 1, profile_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  if (profile.contains("mappings") && profile["mappings"].is_array()) {
    if (sqlite3_prepare_v2(db_,
          "INSERT OR IGNORE INTO profile_mappings "
          "(profile_id, sw_property, target_entity, target_field) VALUES (?,?,?,?);",
          -1, &stmt, 0) == SQLITE_OK) {
      for (const auto& m : profile["mappings"]) {
        sqlite3_bind_int64(stmt, 1, profile_id);
        std::string sp = m.value("sw_property",   "");
        std::string te = m.value("target_entity", "");
        std::string tf = m.value("target_field",  "");
        sqlite3_bind_text(stmt, 2, sp.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, te.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, tf.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
      }
      sqlite3_finalize(stmt);
    }
  }

  // Replace rules
  if (sqlite3_prepare_v2(db_,
        "DELETE FROM profile_rules WHERE profile_id=?;",
        -1, &stmt, 0) == SQLITE_OK) {
    sqlite3_bind_int64(stmt, 1, profile_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  if (profile.contains("rules") && profile["rules"].is_array()) {
    if (sqlite3_prepare_v2(db_,
          "INSERT INTO profile_rules "
          "(profile_id, rule_type, property_name, property_value) VALUES (?,?,?,?);",
          -1, &stmt, 0) == SQLITE_OK) {
      for (const auto& r : profile["rules"]) {
        sqlite3_bind_int64(stmt, 1, profile_id);
        std::string rt = r.value("rule_type",      "");
        std::string pn = r.value("property_name",  "");
        std::string pv = r.value("property_value", "");
        sqlite3_bind_text(stmt, 2, rt.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, pn.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, pv.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
      }
      sqlite3_finalize(stmt);
    }
  }

  tx.Commit();
  return true;
}

}  // namespace sw_dumper::storage
