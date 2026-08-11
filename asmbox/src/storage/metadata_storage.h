// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "auth/auth_utils.h"

struct sqlite3;

namespace sw_dumper::storage {

struct UserInfo {
  int64_t id = -1;
  std::string username;
  bool is_admin = false;
  bool is_active = true;
  std::string created_at;
  std::string last_login_at;
};

struct SessionInfo {
  int64_t user_id = -1;
  std::string username;
  bool is_admin = false;
  int64_t expires_at = 0;
};

struct WorkspaceInfo {
  std::string id;
  std::string display_name;
  std::string created_at;
};

struct WorkspaceRecord {
  std::string id;
  int64_t owner_user_id = -1;
  std::string display_name;
  std::string root_path;
  std::string created_at;
};

struct UserAuthInfo {
  UserInfo user;
  auth::PasswordRecord password;
};

class MetadataStorage {
  sqlite3* db_ = nullptr;
  void Exec(const std::string& sql);

 public:
  explicit MetadataStorage(const std::string& db_path);
  ~MetadataStorage();

  [[nodiscard]] bool CreateUser(const std::string& username,
                                const auth::PasswordRecord& password,
                                bool is_admin = false);
  [[nodiscard]] std::optional<UserAuthInfo> GetUserAuthInfo(
      const std::string& username);
  [[nodiscard]] std::optional<UserInfo> GetUserById(int64_t user_id);
  [[nodiscard]] std::vector<UserInfo> GetUsers();
  [[nodiscard]] int64_t CountUsers();
  [[nodiscard]] int64_t CountActiveAdmins();
  [[nodiscard]] bool SetUserAdmin(int64_t user_id, bool is_admin);
  [[nodiscard]] bool UpdateUser(int64_t user_id,
                                const std::string& username,
                                bool is_admin,
                                bool is_active);
  [[nodiscard]] bool UpdateUserPassword(
      int64_t user_id,
      const auth::PasswordRecord& password);
  [[nodiscard]] bool DeleteUser(int64_t user_id);
  [[nodiscard]] bool UpdateLastLogin(int64_t user_id, int64_t now_epoch_seconds);

  [[nodiscard]] bool CreateSession(int64_t user_id,
                                   const std::string& token_hash,
                                   int64_t expires_at,
                                   int64_t now_epoch_seconds);
  [[nodiscard]] std::optional<SessionInfo> GetSession(
      const std::string& token_hash,
      int64_t now_epoch_seconds);
  [[nodiscard]] bool DeleteSession(const std::string& token_hash);
  [[nodiscard]] bool DeleteExpiredSessions(int64_t now_epoch_seconds);

  [[nodiscard]] bool CreateWorkspace(int64_t owner_user_id,
                                     const std::string& workspace_id,
                                     const std::string& display_name,
                                     const std::string& root_path);
  [[nodiscard]] std::vector<WorkspaceInfo> GetWorkspaces(int64_t owner_user_id);
  [[nodiscard]] std::optional<WorkspaceRecord> GetWorkspace(
      int64_t owner_user_id,
      const std::string& workspace_id);
  [[nodiscard]] bool DeleteWorkspace(int64_t owner_user_id,
                                     const std::string& workspace_id);

  [[nodiscard]] nlohmann::json GetProfiles(int64_t owner_user_id);
  [[nodiscard]] int64_t CreateProfile(int64_t owner_user_id,
                                      const std::string& name,
                                      const std::string& description);
  [[nodiscard]] bool DeleteProfile(int64_t owner_user_id, int64_t profile_id);
  [[nodiscard]] nlohmann::json GetProfile(int64_t owner_user_id,
                                          int64_t profile_id);
  [[nodiscard]] bool SaveProfile(int64_t owner_user_id,
                                 const nlohmann::json& profile);
};

}  // namespace sw_dumper::storage
