// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#pragma once

// internal/decompressor.h — zlib raw-deflate decompression wrapper.

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace openswx::internal {

// Decompresses raw deflate data (no zlib header, wbits = -15).
// Returns nullopt if decompression fails (corrupt data or wrong format).
[[nodiscard]] std::optional<std::vector<uint8_t>> InflateRaw(
    std::span<const uint8_t> compressed);

// Decompresses a ZLB block:
//   Offset  0  : 16-byte identification hash (ignored)
//   Offset 16  : uint32 LE uncompressed size
//   Offset 20  : uint32 LE compressed size
//   Offset 24  : raw deflate data
// Returns nullopt if the block is too short or decompression fails.
[[nodiscard]] std::optional<std::vector<uint8_t>> InflateZlb(
    std::span<const uint8_t> zlb_block);

}  // namespace openswx::internal
