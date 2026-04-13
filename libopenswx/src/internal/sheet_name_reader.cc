// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#include "internal/sheet_name_reader.h"

#include "internal/byte_reader.h"

namespace openswx::internal {

std::vector<std::string> ParseSheetNames(std::span<const uint8_t> data) {
  if (data.empty()) return {};

  ByteReader reader(data);

  auto count_opt = reader.ReadU16Le();
  if (!count_opt) return {};
  const uint16_t count = *count_opt;

  std::vector<std::string> names;
  names.reserve(count);

  for (uint16_t i = 0; i < count; ++i) {
    // Each entry starts with FF FE (2-byte marker).
    auto m0 = reader.ReadU8();
    auto m1 = reader.ReadU8();
    if (!m0 || !m1 || *m0 != 0xFF || *m1 != 0xFE) break;

    // 0xFF string type marker.
    auto type_marker = reader.ReadU8();
    if (!type_marker || *type_marker != 0xFF) break;

    // Number of UTF-16LE code units.
    auto len_opt = reader.ReadU8();
    if (!len_opt) break;
    const uint8_t len = *len_opt;

    auto name_opt = reader.ReadUtf16Le(len);
    if (!name_opt) break;
    names.push_back(std::move(*name_opt));
  }

  return names;
}

}  // namespace openswx::internal
