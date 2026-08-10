// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace sw_dumper::auth {

struct PasswordRecord {
  std::string algorithm;
  int iterations = 0;
  std::string salt_hex;
  std::string hash_hex;
};

[[nodiscard]] std::optional<PasswordRecord> HashPassword(
    const std::string& password,
    int iterations = 210000);
[[nodiscard]] bool VerifyPassword(const std::string& password,
                                  const PasswordRecord& record);
[[nodiscard]] std::optional<std::string> GenerateTokenHex(std::size_t bytes);
[[nodiscard]] std::optional<std::string> Sha256Hex(const std::string& input);
[[nodiscard]] std::optional<std::string> GenerateWorkspaceId();

}  // namespace sw_dumper::auth
