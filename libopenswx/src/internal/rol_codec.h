// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#pragma once

// internal/rol_codec.h — ROL (Rotate Left) byte codec for stream name decoding.
//
// SolidWorks 2015+ stream names are ROL-encoded with a per-file key stored at
// byte 7 of the file header.  The inverse operation (ROR) is not needed because
// ROL(ROL(b, shift), 8 - shift) == b, making the codec self-inverse for
// full-byte rotation.

#include <cstdint>
#include <span>
#include <string>

namespace openswx::internal {

// Rotate byte b left by shift bits (shift is masked to 0..7).
[[nodiscard]] constexpr uint8_t RolByte(uint8_t b, int shift) noexcept {
  shift &= 7;
  if (shift == 0) return b;
  return static_cast<uint8_t>((b << shift) | (b >> (8 - shift)));
}

// Decode a ROL-encoded byte sequence into a UTF-8 string.
// Invalid UTF-8 bytes are replaced with '?'.
[[nodiscard]] inline std::string RolDecode(std::span<const uint8_t> encoded,
                                            int key) {
  std::string result;
  result.reserve(encoded.size());
  for (uint8_t b : encoded) result += static_cast<char>(RolByte(b, key));
  return result;
}

// Returns true iff the decoded string is a plausible stream name:
// printable ASCII only (no control characters, no non-ASCII bytes).
[[nodiscard]] inline bool IsValidStreamName(std::string_view name) noexcept {
  if (name.empty()) return false;
  for (char c : name) {
    unsigned char uc = static_cast<unsigned char>(c);
    if (uc < 0x20 || uc >= 0x80) return false;
  }
  return true;
}

}  // namespace openswx::internal
