// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#include "storage/metadata_storage.h"

#include <plog/Log.h>
#include <sqlite3.h>

#include <filesystem>
#include <string>

#include "storage/sqlite_helpers.h"

namespace sw_dumper::storage {
namespace {

class SqlTransaction {
  sqlite3* db_;
  bool committed_ = false;

 public:
  explicit SqlTransaction(sqlite3* db) : db_(db) {
    if (sqlite3_get_autocommit(db_) == 0) {
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    }
    sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
  }

  ~SqlTransaction() {
    if (!committed_) {
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    }
  }

  void Commit() {
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK) {
      committed_ = true;
    }
  }
};

}  // namespace

void MetadataStorage::Exec(const std::string& sql) {
  char* err = nullptr;
  if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
    PLOG_ERROR << "SQL Error: " << (err ? err : "")
               << " in: " << sql.substr(0, 30);
    if (err != nullptr) {
      sqlite3_free(err);
    }
  }
}

MetadataStorage::MetadataStorage(const std::string& db_path) {
  try {
    std::filesystem::create_directories(
        std::filesystem::path(db_path).parent_path());
  } catch (...) {
  }
  if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
    PLOG_FATAL << "Metadata DB open error";
    db_ = nullptr;
    return;
  }

  Exec("PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL; "
       "PRAGMA foreign_keys = ON;");
  Exec(R"SQL(
    CREATE TABLE IF NOT EXISTS users (
      id                  INTEGER PRIMARY KEY AUTOINCREMENT,
      username            TEXT NOT NULL UNIQUE,
      password_algo       TEXT NOT NULL,
      password_iterations INTEGER NOT NULL,
      password_salt       TEXT NOT NULL,
      password_hash       TEXT NOT NULL,
      is_admin            INTEGER NOT NULL DEFAULT 0,
      created_at          DATETIME DEFAULT CURRENT_TIMESTAMP,
      last_login_at       DATETIME
    );
    CREATE TABLE IF NOT EXISTS sessions (
      id                 INTEGER PRIMARY KEY AUTOINCREMENT,
      user_id            INTEGER NOT NULL,
      session_token_hash TEXT NOT NULL UNIQUE,
      expires_at         INTEGER NOT NULL,
      created_at         DATETIME DEFAULT CURRENT_TIMESTAMP,
      last_seen_at       INTEGER NOT NULL,
      FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
    );
    CREATE TABLE IF NOT EXISTS workspaces (
      id             TEXT PRIMARY KEY,
      owner_user_id  INTEGER NOT NULL,
      display_name   TEXT NOT NULL,
      root_path      TEXT NOT NULL,
      created_at     DATETIME DEFAULT CURRENT_TIMESTAMP,
      FOREIGN KEY (owner_user_id) REFERENCES users(id) ON DELETE CASCADE
    );
    CREATE TABLE IF NOT EXISTS profiles (
      id            INTEGER PRIMARY KEY AUTOINCREMENT,
      owner_user_id INTEGER NOT NULL,
      name          TEXT NOT NULL,
      description   TEXT NOT NULL DEFAULT '',
      created_at    DATETIME DEFAULT CURRENT_TIMESTAMP,
      UNIQUE(owner_user_id, name),
      FOREIGN KEY (owner_user_id) REFERENCES users(id) ON DELETE CASCADE
    );
    CREATE TABLE IF NOT EXISTS profile_mappings (
      id            INTEGER PRIMARY KEY AUTOINCREMENT,
      profile_id    INTEGER NOT NULL,
      sw_property   TEXT NOT NULL,
      target_entity TEXT NOT NULL,
      target_field  TEXT NOT NULL,
      UNIQUE(profile_id, target_entity, target_field),
      FOREIGN KEY (profile_id) REFERENCES profiles(id) ON DELETE CASCADE
    );
    CREATE TABLE IF NOT EXISTS profile_rules (
      id             INTEGER PRIMARY KEY AUTOINCREMENT,
      profile_id     INTEGER NOT NULL,
      rule_type      TEXT NOT NULL,
      property_name  TEXT NOT NULL,
      property_value TEXT NOT NULL,
      FOREIGN KEY (profile_id) REFERENCES profiles(id) ON DELETE CASCADE
    );
    CREATE INDEX IF NOT EXISTS idx_sessions_token ON sessions(session_token_hash);
    CREATE INDEX IF NOT EXISTS idx_sessions_user  ON sessions(user_id);
    CREATE INDEX IF NOT EXISTS idx_workspaces_owner ON workspaces(owner_user_id, created_at DESC);
    CREATE INDEX IF NOT EXISTS idx_profiles_owner ON profiles(owner_user_id, name);
    CREATE INDEX IF NOT EXISTS idx_profile_map_pid ON profile_mappings(profile_id);
    CREATE INDEX IF NOT EXISTS idx_profile_rule_pid ON profile_rules(profile_id);
  )SQL");
}

MetadataStorage::~MetadataStorage() {
  if (db_ != nullptr) {
    sqlite3_close(db_);
  }
}

bool MetadataStorage::CreateUser(const std::string& username,
                                 const auth::PasswordRecord& password) {
  if (db_ == nullptr || username.empty()) {
    return false;
  }
  sqlite3_stmt* stmt = nullptr;
  constexpr char kSql[] =
      "INSERT INTO users "
      "(username, password_algo, password_iterations, password_salt, "
      " password_hash) VALUES (?, ?, ?, ?, ?);";
  if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, password.algorithm.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 3, password.iterations);
  sqlite3_bind_text(stmt, 4, password.salt_hex.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, password.hash_hex.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  return ok;
}

std::optional<UserAuthInfo> MetadataStorage::GetUserAuthInfo(
    const std::string& username) {
  if (db_ == nullptr) {
    return std::nullopt;
  }
  sqlite3_stmt* stmt = nullptr;
  constexpr char kSql[] =
      "SELECT id, username, is_admin, created_at, last_login_at, password_algo, "
      "password_iterations, password_salt, password_hash "
      "FROM users WHERE username=?;";
  if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return std::nullopt;
  }

  UserAuthInfo info;
  info.user.id = sqlite3_column_int64(stmt, 0);
  info.user.username = SafeColumnText(stmt, 1);
  info.user.is_admin = sqlite3_column_int(stmt, 2) != 0;
  info.user.created_at = SafeColumnText(stmt, 3);
  info.user.last_login_at = SafeColumnText(stmt, 4);
  info.password.algorithm = SafeColumnText(stmt, 5);
  info.password.iterations = sqlite3_column_int(stmt, 6);
  info.password.salt_hex = SafeColumnText(stmt, 7);
  info.password.hash_hex = SafeColumnText(stmt, 8);
  sqlite3_finalize(stmt);
  return info;
}

std::optional<UserInfo> MetadataStorage::GetUserById(int64_t user_id) {
  if (db_ == nullptr) {
    return std::nullopt;
  }
  sqlite3_stmt* stmt = nullptr;
  constexpr char kSql[] =
      "SELECT id, username, is_admin, created_at, last_login_at "
      "FROM users WHERE id=?;";
  if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_int64(stmt, 1, user_id);
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return std::nullopt;
  }

  UserInfo user;
  user.id = sqlite3_column_int64(stmt, 0);
  user.username = SafeColumnText(stmt, 1);
  user.is_admin = sqlite3_column_int(stmt, 2) != 0;
  user.created_at = SafeColumnText(stmt, 3);
  user.last_login_at = SafeColumnText(stmt, 4);
  sqlite3_finalize(stmt);
  return user;
}

bool MetadataStorage::UpdateLastLogin(int64_t user_id,
                                      int64_t now_epoch_seconds) {
  if (db_ == nullptr) {
    return false;
  }
  sqlite3_stmt* stmt = nullptr;
  constexpr char kSql[] =
      "UPDATE users SET last_login_at=datetime(?,'unixepoch') WHERE id=?;";
  if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(stmt, 1, now_epoch_seconds);
  sqlite3_bind_int64(stmt, 2, user_id);
  const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  return ok;
}

bool MetadataStorage::CreateSession(int64_t user_id,
                                    const std::string& token_hash,
                                    int64_t expires_at,
                                    int64_t now_epoch_seconds) {
  if (db_ == nullptr || token_hash.empty()) {
    return false;
  }
  sqlite3_stmt* stmt = nullptr;
  constexpr char kSql[] =
      "INSERT INTO sessions (user_id, session_token_hash, expires_at, "
      "last_seen_at) VALUES (?, ?, ?, ?);";
  if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(stmt, 1, user_id);
  sqlite3_bind_text(stmt, 2, token_hash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 3, expires_at);
  sqlite3_bind_int64(stmt, 4, now_epoch_seconds);
  const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  return ok;
}

std::optional<SessionInfo> MetadataStorage::GetSession(
    const std::string& token_hash,
    int64_t now_epoch_seconds) {
  if (db_ == nullptr) {
    return std::nullopt;
  }
  sqlite3_stmt* stmt = nullptr;
  constexpr char kSql[] =
      "SELECT s.user_id, u.username, u.is_admin, s.expires_at "
      "FROM sessions s "
      "JOIN users u ON u.id=s.user_id "
      "WHERE s.session_token_hash=? AND s.expires_at>?;";
  if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_text(stmt, 1, token_hash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, now_epoch_seconds);
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return std::nullopt;
  }

  SessionInfo session;
  session.user_id = sqlite3_column_int64(stmt, 0);
  session.username = SafeColumnText(stmt, 1);
  session.is_admin = sqlite3_column_int(stmt, 2) != 0;
  session.expires_at = sqlite3_column_int64(stmt, 3);
  sqlite3_finalize(stmt);

  if (sqlite3_prepare_v2(
          db_, "UPDATE sessions SET last_seen_at=? WHERE session_token_hash=?;",
          -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_int64(stmt, 1, now_epoch_seconds);
    sqlite3_bind_text(stmt, 2, token_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  return session;
}

bool MetadataStorage::DeleteSession(const std::string& token_hash) {
  if (db_ == nullptr) {
    return false;
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(
          db_, "DELETE FROM sessions WHERE session_token_hash=?;", -1, &stmt,
          nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(stmt, 1, token_hash.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  return ok;
}

bool MetadataStorage::DeleteExpiredSessions(int64_t now_epoch_seconds) {
  if (db_ == nullptr) {
    return false;
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "DELETE FROM sessions WHERE expires_at<=?;", -1,
                         &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(stmt, 1, now_epoch_seconds);
  const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  return ok;
}

bool MetadataStorage::CreateWorkspace(int64_t owner_user_id,
                                      const std::string& workspace_id,
                                      const std::string& display_name,
                                      const std::string& root_path) {
  if (db_ == nullptr || workspace_id.empty() || root_path.empty()) {
    return false;
  }
  sqlite3_stmt* stmt = nullptr;
  constexpr char kSql[] =
      "INSERT INTO workspaces (id, owner_user_id, display_name, root_path) "
      "VALUES (?, ?, ?, ?);";
  if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(stmt, 1, workspace_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, owner_user_id);
  sqlite3_bind_text(stmt, 3, display_name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, root_path.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  return ok;
}

std::vector<WorkspaceInfo> MetadataStorage::GetWorkspaces(int64_t owner_user_id) {
  std::vector<WorkspaceInfo> workspaces;
  if (db_ == nullptr) {
    return workspaces;
  }
  sqlite3_stmt* stmt = nullptr;
  constexpr char kSql[] =
      "SELECT id, display_name, created_at FROM workspaces "
      "WHERE owner_user_id=? ORDER BY created_at DESC;";
  if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
    return workspaces;
  }
  sqlite3_bind_int64(stmt, 1, owner_user_id);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    WorkspaceInfo info;
    info.id = SafeColumnText(stmt, 0);
    info.display_name = SafeColumnText(stmt, 1);
    info.created_at = SafeColumnText(stmt, 2);
    workspaces.push_back(info);
  }
  sqlite3_finalize(stmt);
  return workspaces;
}

std::optional<WorkspaceRecord> MetadataStorage::GetWorkspace(
    int64_t owner_user_id,
    const std::string& workspace_id) {
  if (db_ == nullptr) {
    return std::nullopt;
  }
  sqlite3_stmt* stmt = nullptr;
  constexpr char kSql[] =
      "SELECT id, owner_user_id, display_name, root_path, created_at "
      "FROM workspaces WHERE owner_user_id=? AND id=?;";
  if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_int64(stmt, 1, owner_user_id);
  sqlite3_bind_text(stmt, 2, workspace_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return std::nullopt;
  }

  WorkspaceRecord record;
  record.id = SafeColumnText(stmt, 0);
  record.owner_user_id = sqlite3_column_int64(stmt, 1);
  record.display_name = SafeColumnText(stmt, 2);
  record.root_path = SafeColumnText(stmt, 3);
  record.created_at = SafeColumnText(stmt, 4);
  sqlite3_finalize(stmt);
  return record;
}

bool MetadataStorage::DeleteWorkspace(int64_t owner_user_id,
                                      const std::string& workspace_id) {
  if (db_ == nullptr) {
    return false;
  }
  sqlite3_stmt* stmt = nullptr;
  constexpr char kSql[] =
      "DELETE FROM workspaces WHERE owner_user_id=? AND id=?;";
  if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(stmt, 1, owner_user_id);
  sqlite3_bind_text(stmt, 2, workspace_id.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  return ok;
}

nlohmann::json MetadataStorage::GetProfiles(int64_t owner_user_id) {
  nlohmann::json result = nlohmann::json::array();
  if (db_ == nullptr) {
    return result;
  }
  sqlite3_stmt* stmt = nullptr;
  constexpr char kSql[] =
      "SELECT id, name, description, created_at FROM profiles "
      "WHERE owner_user_id=? ORDER BY name;";
  if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
    return result;
  }
  sqlite3_bind_int64(stmt, 1, owner_user_id);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    nlohmann::json profile;
    profile["id"] = sqlite3_column_int64(stmt, 0);
    profile["name"] = SafeColumnText(stmt, 1);
    profile["description"] = SafeColumnText(stmt, 2);
    profile["created_at"] = SafeColumnText(stmt, 3);
    result.push_back(profile);
  }
  sqlite3_finalize(stmt);
  return result;
}

int64_t MetadataStorage::CreateProfile(int64_t owner_user_id,
                                       const std::string& name,
                                       const std::string& description) {
  if (db_ == nullptr || name.empty()) {
    return -1;
  }
  sqlite3_stmt* stmt = nullptr;
  constexpr char kSql[] =
      "INSERT INTO profiles (owner_user_id, name, description) VALUES (?, ?, ?);";
  if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
    return -1;
  }
  sqlite3_bind_int64(stmt, 1, owner_user_id);
  sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, description.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    return -1;
  }
  const int64_t id = sqlite3_last_insert_rowid(db_);
  sqlite3_finalize(stmt);
  return id;
}

bool MetadataStorage::DeleteProfile(int64_t owner_user_id, int64_t profile_id) {
  if (db_ == nullptr) {
    return false;
  }
  sqlite3_stmt* stmt = nullptr;
  constexpr char kSql[] =
      "DELETE FROM profiles WHERE owner_user_id=? AND id=?;";
  if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(stmt, 1, owner_user_id);
  sqlite3_bind_int64(stmt, 2, profile_id);
  const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  return ok;
}

nlohmann::json MetadataStorage::GetProfile(int64_t owner_user_id,
                                           int64_t profile_id) {
  nlohmann::json result;
  if (db_ == nullptr) {
    return result;
  }

  sqlite3_stmt* stmt = nullptr;
  constexpr char kHeaderSql[] =
      "SELECT id, name, description, created_at FROM profiles "
      "WHERE owner_user_id=? AND id=?;";
  if (sqlite3_prepare_v2(db_, kHeaderSql, -1, &stmt, nullptr) != SQLITE_OK) {
    return result;
  }
  sqlite3_bind_int64(stmt, 1, owner_user_id);
  sqlite3_bind_int64(stmt, 2, profile_id);
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return result;
  }
  result["id"] = sqlite3_column_int64(stmt, 0);
  result["name"] = SafeColumnText(stmt, 1);
  result["description"] = SafeColumnText(stmt, 2);
  result["created_at"] = SafeColumnText(stmt, 3);
  sqlite3_finalize(stmt);

  result["mappings"] = nlohmann::json::array();
  constexpr char kMappingsSql[] =
      "SELECT sw_property, target_entity, target_field "
      "FROM profile_mappings WHERE profile_id=? ORDER BY target_entity, target_field;";
  if (sqlite3_prepare_v2(db_, kMappingsSql, -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_int64(stmt, 1, profile_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      result["mappings"].push_back({
          {"sw_property", SafeColumnText(stmt, 0)},
          {"target_entity", SafeColumnText(stmt, 1)},
          {"target_field", SafeColumnText(stmt, 2)},
      });
    }
    sqlite3_finalize(stmt);
  }

  result["rules"] = nlohmann::json::array();
  constexpr char kRulesSql[] =
      "SELECT rule_type, property_name, property_value "
      "FROM profile_rules WHERE profile_id=? ORDER BY rule_type, property_name;";
  if (sqlite3_prepare_v2(db_, kRulesSql, -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_int64(stmt, 1, profile_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      result["rules"].push_back({
          {"rule_type", SafeColumnText(stmt, 0)},
          {"property_name", SafeColumnText(stmt, 1)},
          {"property_value", SafeColumnText(stmt, 2)},
      });
    }
    sqlite3_finalize(stmt);
  }

  return result;
}

bool MetadataStorage::SaveProfile(int64_t owner_user_id,
                                  const nlohmann::json& profile) {
  if (db_ == nullptr || !profile.is_object()) {
    return false;
  }
  const int64_t profile_id = profile.value("id", int64_t(-1));
  if (profile_id < 0) {
    return false;
  }
  sqlite3_stmt* stmt = nullptr;
  constexpr char kExistsSql[] =
      "SELECT COUNT(*) FROM profiles WHERE owner_user_id=? AND id=?;";
  if (sqlite3_prepare_v2(db_, kExistsSql, -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(stmt, 1, owner_user_id);
  sqlite3_bind_int64(stmt, 2, profile_id);
  const bool exists =
      sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1;
  sqlite3_finalize(stmt);
  if (!exists) {
    return false;
  }

  SqlTransaction tx(db_);
  constexpr char kUpdateSql[] =
      "UPDATE profiles SET name=?, description=? WHERE owner_user_id=? AND id=?;";
  if (sqlite3_prepare_v2(db_, kUpdateSql, -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  const std::string name = profile.value("name", "");
  const std::string description = profile.value("description", "");
  sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, description.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 3, owner_user_id);
  sqlite3_bind_int64(stmt, 4, profile_id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (sqlite3_prepare_v2(
          db_, "DELETE FROM profile_mappings WHERE profile_id=?;", -1, &stmt,
          nullptr) == SQLITE_OK) {
    sqlite3_bind_int64(stmt, 1, profile_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  if (profile.contains("mappings") && profile["mappings"].is_array()) {
    constexpr char kInsertMappingsSql[] =
        "INSERT OR IGNORE INTO profile_mappings "
        "(profile_id, sw_property, target_entity, target_field) "
        "VALUES (?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db_, kInsertMappingsSql, -1, &stmt, nullptr) ==
        SQLITE_OK) {
      for (const auto& mapping : profile["mappings"]) {
        const std::string sw_property = mapping.value("sw_property", "");
        const std::string target_entity = mapping.value("target_entity", "");
        const std::string target_field = mapping.value("target_field", "");
        sqlite3_bind_int64(stmt, 1, profile_id);
        sqlite3_bind_text(stmt, 2, sw_property.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, target_entity.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, target_field.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
      }
      sqlite3_finalize(stmt);
    }
  }

  if (sqlite3_prepare_v2(
          db_, "DELETE FROM profile_rules WHERE profile_id=?;", -1, &stmt,
          nullptr) == SQLITE_OK) {
    sqlite3_bind_int64(stmt, 1, profile_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  if (profile.contains("rules") && profile["rules"].is_array()) {
    constexpr char kInsertRulesSql[] =
        "INSERT INTO profile_rules "
        "(profile_id, rule_type, property_name, property_value) "
        "VALUES (?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db_, kInsertRulesSql, -1, &stmt, nullptr) ==
        SQLITE_OK) {
      for (const auto& rule : profile["rules"]) {
        const std::string rule_type = rule.value("rule_type", "");
        const std::string property_name = rule.value("property_name", "");
        const std::string property_value = rule.value("property_value", "");
        sqlite3_bind_int64(stmt, 1, profile_id);
        sqlite3_bind_text(stmt, 2, rule_type.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, property_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, property_value.c_str(), -1,
                          SQLITE_TRANSIENT);
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
