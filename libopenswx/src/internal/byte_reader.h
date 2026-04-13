// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#pragma once

// internal/byte_reader.h — Bounds-checked, cursor-based binary reader.
//
// All multi-byte reads are little-endian, matching the SolidWorks file format.
// Read methods return std::nullopt on any out-of-bounds access; callers must
// check before using the returned value.

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace openswx::internal {

class ByteReader {
 public:
  explicit ByteReader(std::span<const uint8_t> data) noexcept
      : data_(data), pos_(0) {}

  // Returns the number of bytes not yet consumed.
  [[nodiscard]] std::size_t remaining() const noexcept {
    return pos_ <= data_.size() ? data_.size() - pos_ : 0;
  }

  [[nodiscard]] std::size_t pos() const noexcept { return pos_; }

  [[nodiscard]] bool CanRead(std::size_t n) const noexcept {
    return pos_ + n <= data_.size();
  }

  // Advance the cursor without returning data.
  // Has no effect if n would exceed the buffer.
  void Skip(std::size_t n) noexcept {
    if (CanRead(n)) pos_ += n;
  }

  // Seek to an absolute offset. Has no effect if offset > buffer size.
  void SeekTo(std::size_t offset) noexcept {
    if (offset <= data_.size()) pos_ = offset;
  }

  [[nodiscard]] std::optional<uint8_t> ReadU8() noexcept {
    if (!CanRead(1)) return std::nullopt;
    return data_[pos_++];
  }

  [[nodiscard]] std::optional<uint16_t> ReadU16Le() noexcept {
    if (!CanRead(2)) return std::nullopt;
    uint16_t v = static_cast<uint16_t>(data_[pos_]) |
                 (static_cast<uint16_t>(data_[pos_ + 1]) << 8);
    pos_ += 2;
    return v;
  }

  [[nodiscard]] std::optional<uint32_t> ReadU32Le() noexcept {
    if (!CanRead(4)) return std::nullopt;
    uint32_t v = static_cast<uint32_t>(data_[pos_]) |
                 (static_cast<uint32_t>(data_[pos_ + 1]) << 8) |
                 (static_cast<uint32_t>(data_[pos_ + 2]) << 16) |
                 (static_cast<uint32_t>(data_[pos_ + 3]) << 24);
    pos_ += 4;
    return v;
  }

  [[nodiscard]] std::optional<uint64_t> ReadU64Le() noexcept {
    if (!CanRead(8)) return std::nullopt;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
      v |= static_cast<uint64_t>(data_[pos_ + i]) << (8 * i);
    pos_ += 8;
    return v;
  }

  // Returns a view of the next n bytes without copying.
  // Returns an empty span on bounds violation.
  [[nodiscard]] std::span<const uint8_t> ReadSpan(std::size_t n) noexcept {
    if (!CanRead(n)) return {};
    auto s = data_.subspan(pos_, n);
    pos_ += n;
    return s;
  }

  // Returns a copy of the next n bytes.
  [[nodiscard]] std::optional<std::vector<uint8_t>> ReadBytes(
      std::size_t n) {
    if (!CanRead(n)) return std::nullopt;
    std::vector<uint8_t> buf(data_.begin() + pos_,
                              data_.begin() + pos_ + n);
    pos_ += n;
    return buf;
  }

  // Reads n UTF-16LE code units and converts to UTF-8.
  // Handles BMP characters only (U+0000..U+FFFF); surrogates are skipped.
  [[nodiscard]] std::optional<std::string> ReadUtf16Le(std::size_t n_units) {
    if (!CanRead(n_units * 2)) return std::nullopt;
    std::string result;
    result.reserve(n_units);
    for (std::size_t i = 0; i < n_units; ++i) {
      uint16_t cp = static_cast<uint16_t>(data_[pos_]) |
                    (static_cast<uint16_t>(data_[pos_ + 1]) << 8);
      pos_ += 2;
      if (cp == 0) continue;  // null terminator
      if (cp < 0x80) {
        result += static_cast<char>(cp);
      } else if (cp < 0x800) {
        result += static_cast<char>(0xC0 | (cp >> 6));
        result += static_cast<char>(0x80 | (cp & 0x3F));
      } else {
        result += static_cast<char>(0xE0 | (cp >> 12));
        result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        result += static_cast<char>(0x80 | (cp & 0x3F));
      }
    }
    return result;
  }

 private:
  std::span<const uint8_t> data_;
  std::size_t pos_;
};

// Reads a uint32_t LE at an absolute offset within a buffer.
// Returns 0xFFFFFFFF on bounds violation.
[[nodiscard]] inline uint32_t ReadU32LeAt(std::span<const uint8_t> buf,
                                           std::size_t offset) noexcept {
  if (offset + 4 > buf.size()) return 0xFFFFFFFF;
  return static_cast<uint32_t>(buf[offset]) |
         (static_cast<uint32_t>(buf[offset + 1]) << 8) |
         (static_cast<uint32_t>(buf[offset + 2]) << 16) |
         (static_cast<uint32_t>(buf[offset + 3]) << 24);
}

}  // namespace openswx::internal
