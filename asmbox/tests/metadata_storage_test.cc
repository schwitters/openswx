// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#include <gtest/gtest.h>

#include <filesystem>
#include <random>
#include <string>

#include "auth/auth_utils.h"
#include "storage/metadata_storage.h"

namespace fs = std::filesystem;

namespace sw_dumper::storage {
namespace {

class TempDirectory {
 public:
  TempDirectory() {
    const auto nonce = std::to_string(std::random_device{}());
    path_ = fs::temp_directory_path() / ("openswx_metadata_test_" + nonce);
    fs::create_directories(path_);
  }

  ~TempDirectory() {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }

  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

TEST(MetadataStorageTest, UserSessionWorkspaceAndProfileLifecycle) {
  TempDirectory temp_directory;
  MetadataStorage storage((temp_directory.path() / "main.sqlite").string());

  const auto password = auth::HashPassword("hunter2hunter2", 1000);
  ASSERT_TRUE(password.has_value());
  ASSERT_TRUE(storage.CreateUser("alice", *password));

  const auto auth_info = storage.GetUserAuthInfo("alice");
  ASSERT_TRUE(auth_info.has_value());
  EXPECT_TRUE(auth::VerifyPassword("hunter2hunter2", auth_info->password));

  const auto token_hash = auth::Sha256Hex("session-token");
  ASSERT_TRUE(token_hash.has_value());
  ASSERT_TRUE(storage.CreateSession(auth_info->user.id, *token_hash, 2000, 1000));

  const auto session = storage.GetSession(*token_hash, 1500);
  ASSERT_TRUE(session.has_value());
  EXPECT_EQ(session->username, "alice");
  EXPECT_EQ(session->user_id, auth_info->user.id);

  ASSERT_TRUE(storage.CreateWorkspace(auth_info->user.id, "ws-1", "Demo",
                                      (temp_directory.path() / "ws-1").string()));
  const auto workspaces = storage.GetWorkspaces(auth_info->user.id);
  ASSERT_EQ(workspaces.size(), 1u);
  EXPECT_EQ(workspaces[0].id, "ws-1");
  EXPECT_EQ(workspaces[0].display_name, "Demo");

  const auto workspace = storage.GetWorkspace(auth_info->user.id, "ws-1");
  ASSERT_TRUE(workspace.has_value());
  EXPECT_EQ(workspace->display_name, "Demo");

  const int64_t profile_id =
      storage.CreateProfile(auth_info->user.id, "Default", "User profile");
  ASSERT_GT(profile_id, 0);
  nlohmann::json profile_update{
      {"id", profile_id},
      {"name", "Default"},
      {"description", "User profile"},
      {"mappings",
       {{{"sw_property", "PartNumber"},
         {"target_entity", "part"},
         {"target_field", "number"}}}},
      {"rules",
       {{{"rule_type", "non_bom"},
         {"property_name", "Type"},
         {"property_value", "Reference"}}}},
  };
  ASSERT_TRUE(storage.SaveProfile(auth_info->user.id, profile_update));

  const auto profile = storage.GetProfile(auth_info->user.id, profile_id);
  ASSERT_TRUE(profile.contains("mappings"));
  ASSERT_TRUE(profile.contains("rules"));
  EXPECT_EQ(profile["mappings"].size(), 1u);
  EXPECT_EQ(profile["rules"].size(), 1u);
}

}  // namespace
}  // namespace sw_dumper::storage
