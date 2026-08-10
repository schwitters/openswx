// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#include "auth/auth_utils.h"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <array>
#include <cstdint>
#include <sstream>
#include <vector>

namespace sw_dumper::auth {
namespace {

constexpr int kSaltBytes = 16;
constexpr int kHashBytes = 32;
constexpr char kAlgorithm[] = "pbkdf2_sha256";

std::string BytesToHex(const uint8_t* data, std::size_t size) {
  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(size * 2);
  for (std::size_t i = 0; i < size; ++i) {
    hex.push_back(kHexDigits[(data[i] >> 4) & 0x0F]);
    hex.push_back(kHexDigits[data[i] & 0x0F]);
  }
  return hex;
}

std::optional<std::vector<uint8_t>> HexToBytes(const std::string& hex) {
  if (hex.size() % 2 != 0) {
    return std::nullopt;
  }
  auto hex_value = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') {
      return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
      return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
      return 10 + (ch - 'A');
    }
    return -1;
  };

  std::vector<uint8_t> bytes;
  bytes.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const int high = hex_value(hex[i]);
    const int low = hex_value(hex[i + 1]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    bytes.push_back(static_cast<uint8_t>((high << 4) | low));
  }
  return bytes;
}

bool ConstantTimeEquals(const std::string& lhs, const std::string& rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  unsigned char diff = 0;
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    diff |= static_cast<unsigned char>(lhs[i] ^ rhs[i]);
  }
  return diff == 0;
}

std::optional<std::string> DerivePasswordHash(const std::string& password,
                                              const std::string& salt_hex,
                                              int iterations) {
  if (iterations <= 0) {
    return std::nullopt;
  }
  const auto salt_bytes = HexToBytes(salt_hex);
  if (!salt_bytes) {
    return std::nullopt;
  }

  std::array<uint8_t, kHashBytes> hash{};
  if (PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()),
                        salt_bytes->data(),
                        static_cast<int>(salt_bytes->size()), iterations,
                        EVP_sha256(), static_cast<int>(hash.size()),
                        hash.data()) != 1) {
    return std::nullopt;
  }
  return BytesToHex(hash.data(), hash.size());
}

}  // namespace

std::optional<PasswordRecord> HashPassword(const std::string& password,
                                           int iterations) {
  if (password.empty() || iterations <= 0) {
    return std::nullopt;
  }

  std::array<uint8_t, kSaltBytes> salt{};
  if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1) {
    return std::nullopt;
  }

  PasswordRecord record;
  record.algorithm = kAlgorithm;
  record.iterations = iterations;
  record.salt_hex = BytesToHex(salt.data(), salt.size());
  const auto hash_hex =
      DerivePasswordHash(password, record.salt_hex, record.iterations);
  if (!hash_hex) {
    return std::nullopt;
  }
  record.hash_hex = *hash_hex;
  return record;
}

bool VerifyPassword(const std::string& password, const PasswordRecord& record) {
  if (record.algorithm != kAlgorithm) {
    return false;
  }
  const auto hash_hex =
      DerivePasswordHash(password, record.salt_hex, record.iterations);
  return hash_hex && ConstantTimeEquals(*hash_hex, record.hash_hex);
}

std::optional<std::string> GenerateTokenHex(std::size_t bytes) {
  if (bytes == 0) {
    return std::nullopt;
  }
  std::vector<uint8_t> buffer(bytes);
  if (RAND_bytes(buffer.data(), static_cast<int>(buffer.size())) != 1) {
    return std::nullopt;
  }
  return BytesToHex(buffer.data(), buffer.size());
}

std::optional<std::string> Sha256Hex(const std::string& input) {
  std::array<uint8_t, SHA256_DIGEST_LENGTH> digest{};
  if (SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(),
             digest.data()) == nullptr) {
    return std::nullopt;
  }
  return BytesToHex(digest.data(), digest.size());
}

std::optional<std::string> GenerateWorkspaceId() {
  return GenerateTokenHex(12);
}

}  // namespace sw_dumper::auth
