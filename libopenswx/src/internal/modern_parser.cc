// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#include "internal/modern_parser.h"

#include <algorithm>
#include <cstdint>

#include "internal/decompressor.h"
#include "internal/rol_codec.h"

namespace openswx::internal {

// ── Format-detection helpers ──────────────────────────────────────────────────

bool IsZipOpc(std::span<const uint8_t> data) noexcept {
  // ZIP local-file-header magic: PK\x03\x04
  return data.size() >= 4 &&
         data[0] == 0x50 && data[1] == 0x4B &&
         data[2] == 0x03 && data[3] == 0x04;
}

bool IsModernSwx(std::span<const uint8_t> data) noexcept {
  // Reject OLE2 and ZIP/OPC — those are handled by their own parsers.
  if (data.size() < 22) return false;  // need at least header + one marker
  if (data[0] == 0xD0 && data[1] == 0xCF &&
      data[2] == 0x11 && data[3] == 0xE0) return false;  // OLE2
  if (data[0] == 0x50 && data[1] == 0x4B &&
      data[2] == 0x03 && data[3] == 0x04) return false;  // ZIP/OPC

  // The 6-byte chunk marker always appears within the first 64 bytes.
  // Empirically offset 13–21 in known SW files; scan up to 64 for safety.
  constexpr std::size_t kSearchLimit = 64;
  const std::size_t limit = std::min(data.size(), kSearchLimit);
  constexpr uint8_t kMarker[] = {0x14, 0x00, 0x06, 0x00, 0x08, 0x00};
  auto it = std::search(data.begin(), data.begin() + limit,
                        std::begin(kMarker), std::end(kMarker));
  return it != data.begin() + limit;
}

bool IsValidStreamName(const std::string& name) noexcept {
  if (name.empty()) return false;
  for (unsigned char c : name)
    if (c >= 0x80) return false;
  return true;
}

// ── Internal chunk-parsing constants ─────────────────────────────────────────

namespace {

// Chunk layout (chunk starts at si = MARKER_PREFIX_pos - 4):
//   si+0x00   val_a   (4 bytes, file-specific — not used for type detection)
//   si+0x04   0x14    (fixed separator byte)
//   si+0x05   00 06 00 08 00  (5 CORE bytes)
//   si+0x0a   section_type   (1 byte: 0xDF=TOC, 0xFD=data, 0x1C=mini, …)
//   si+0x0b   suffix         (3 file-specific bytes)
//   si+0x0e   f1    uint32 LE — >= 65536 for inline chunks
//   si+0x12   csz   uint32 LE — compressed data size
//   si+0x16   usz   uint32 LE — uncompressed data size
//   si+0x1a   nsz   uint32 LE — stream name length in bytes
//   si+0x1e   name[nsz]       — ROL-encoded stream name (UTF-8)
//   si+0x1e+nsz  data[csz]   — raw deflate (inline chunks only)

constexpr std::size_t kChunkHeaderSize = 0x1E;
constexpr uint32_t kInlineF1Threshold = 65536;
constexpr uint32_t kMaxNameSize = 512;
constexpr uint32_t kMaxCompressedSize = 64 * 1024 * 1024;  // 64 MiB sanity cap

// The 6-byte prefix that precedes the section-type byte.
// The full sequence starting at si+0x04 is: 14 00 06 00 08 00.
constexpr uint8_t kMarkerPrefix[] = {0x14, 0x00, 0x06, 0x00, 0x08, 0x00};
constexpr std::size_t kMarkerLen = sizeof(kMarkerPrefix);

// Reads a uint32_t LE at the given absolute byte offset within buf.
[[nodiscard]] uint32_t U32At(std::span<const uint8_t> buf,
                              std::size_t offset) noexcept {
  if (offset + 4 > buf.size()) return 0;
  return static_cast<uint32_t>(buf[offset]) |
         (static_cast<uint32_t>(buf[offset + 1]) << 8) |
         (static_cast<uint32_t>(buf[offset + 2]) << 16) |
         (static_cast<uint32_t>(buf[offset + 3]) << 24);
}

}  // namespace

bool ParseModernFormat(std::span<const uint8_t> data, StreamMap& streams) {
  if (data.size() < 8) return false;

  // ROL key is stored at byte 7 of the file header.
  const int key = static_cast<int>(data[7]);

  std::size_t search_pos = 0;
  while (true) {
    // Find the next occurrence of the 6-byte marker prefix.
    auto it = std::search(data.begin() + search_pos, data.end(),
                          std::begin(kMarkerPrefix), std::end(kMarkerPrefix));
    if (it == data.end()) break;

    const std::size_t marker_pos = static_cast<std::size_t>(it - data.begin());
    // Chunk starts 4 bytes before the marker.
    if (marker_pos < 4) { ++search_pos; continue; }
    const std::size_t si = marker_pos - 4;

    // Ensure the fixed-size chunk header is fully within the buffer.
    if (si + kChunkHeaderSize > data.size()) { ++search_pos; continue; }

    const uint32_t f1  = U32At(data, si + 0x0E);
    const uint32_t csz = U32At(data, si + 0x12);
    const uint32_t nsz = U32At(data, si + 0x1A);

    // Reject obviously invalid chunk headers.
    if (nsz > kMaxNameSize || csz > kMaxCompressedSize) {
      ++search_pos;
      continue;
    }

    const std::size_t name_start = si + kChunkHeaderSize;
    const std::size_t name_end   = name_start + nsz;
    if (name_end > data.size()) { ++search_pos; continue; }

    const std::string name =
        RolDecode(data.subspan(name_start, nsz), key);

    if (!IsValidStreamName(name)) { ++search_pos; continue; }

    const bool is_inline = (f1 >= kInlineF1Threshold);

    if (is_inline && csz > 0) {
      const std::size_t data_start = name_end;
      const std::size_t data_end   = data_start + csz;
      if (data_end <= data.size()) {
        auto decompressed = InflateRaw(data.subspan(data_start, csz));
        if (decompressed && streams.find(name) == streams.end()) {
          streams.emplace(name, std::move(*decompressed));
        }
        // Skip past the entire inline chunk payload.
        search_pos = data_end;
        continue;
      }
    } else if (is_inline && csz == 0) {
      // Empty inline stream.
      if (streams.find(name) == streams.end()) {
        streams.emplace(name, std::vector<uint8_t>{});
      }
    }
    // Non-inline (reference) chunks carry no data — advance past the marker.
    search_pos = marker_pos + kMarkerLen;
  }

  return true;
}

}  // namespace openswx::internal
