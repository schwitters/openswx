// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#include "internal/decompressor.h"

#include <zlib.h>

#include <cstring>
#include <limits>

namespace openswx::internal {

namespace {

std::optional<std::vector<uint8_t>> InflateRawImpl(
    const uint8_t* data, std::size_t size, std::size_t hint_uncompressed) {
  if (size == 0) return std::vector<uint8_t>{};

  // Start with the hinted size, grow as needed.
  std::size_t out_cap =
      hint_uncompressed > 0 ? hint_uncompressed : size * 4;

  std::vector<uint8_t> out(out_cap);

  z_stream strm{};
  strm.next_in = const_cast<Bytef*>(data);
  strm.avail_in = static_cast<uInt>(size);

  // wbits = -15 selects raw deflate (no zlib header).
  if (inflateInit2(&strm, -15) != Z_OK) return std::nullopt;

  int ret = Z_OK;
  while (ret == Z_OK) {
    strm.next_out = out.data() + strm.total_out;
    strm.avail_out = static_cast<uInt>(out.size() - strm.total_out);

    ret = inflate(&strm, Z_NO_FLUSH);
    if (ret == Z_STREAM_END) break;
    if (ret != Z_OK) {
      inflateEnd(&strm);
      return std::nullopt;
    }
    if (strm.avail_out == 0) {
      // Output buffer too small — double it.
      out.resize(out.size() * 2);
    }
  }

  std::size_t actual_size = strm.total_out;
  inflateEnd(&strm);
  out.resize(actual_size);
  return out;
}

}  // namespace

std::optional<std::vector<uint8_t>> InflateRaw(
    std::span<const uint8_t> compressed) {
  return InflateRawImpl(compressed.data(), compressed.size(), 0);
}

std::optional<std::vector<uint8_t>> InflateZlb(
    std::span<const uint8_t> zlb_block) {
  // Minimum: 16 (hash) + 4 (usz) + 4 (csz) = 24 bytes header.
  if (zlb_block.size() < 24) return std::nullopt;

  uint32_t usz = static_cast<uint32_t>(zlb_block[16]) |
                 (static_cast<uint32_t>(zlb_block[17]) << 8) |
                 (static_cast<uint32_t>(zlb_block[18]) << 16) |
                 (static_cast<uint32_t>(zlb_block[19]) << 24);

  uint32_t csz = static_cast<uint32_t>(zlb_block[20]) |
                 (static_cast<uint32_t>(zlb_block[21]) << 8) |
                 (static_cast<uint32_t>(zlb_block[22]) << 16) |
                 (static_cast<uint32_t>(zlb_block[23]) << 24);

  if (24 + csz > zlb_block.size()) return std::nullopt;

  return InflateRawImpl(zlb_block.data() + 24, csz, usz);
}

}  // namespace openswx::internal
