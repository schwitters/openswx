// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#include <gtest/gtest.h>

#include "auth/auth_utils.h"

namespace sw_dumper::auth {
namespace {

TEST(AuthUtilsTest, HashAndVerifyPassword) {
  const auto record = HashPassword("correct horse battery staple", 1000);
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->algorithm, "pbkdf2_sha256");
  EXPECT_TRUE(VerifyPassword("correct horse battery staple", *record));
  EXPECT_FALSE(VerifyPassword("wrong password", *record));
}

TEST(AuthUtilsTest, GenerateTokenHexProducesExpectedLength) {
  const auto token = GenerateTokenHex(24);
  ASSERT_TRUE(token.has_value());
  EXPECT_EQ(token->size(), 48u);
}

TEST(AuthUtilsTest, Sha256HexIsStable) {
  const auto digest = Sha256Hex("openswx");
  ASSERT_TRUE(digest.has_value());
  EXPECT_EQ(*digest,
            "6edf6e641267dd6bd57c2966385554a4589bdb23361f426741c78ea4ad66eba2");
}

}  // namespace
}  // namespace sw_dumper::auth
