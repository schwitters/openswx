// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "internal/decompressor.h"
#include "test_helpers.h"

namespace openswx::internal {
namespace {

TEST(DecompressorTest, InflateRawAcceptsEmptyInput) {
  const auto result = InflateRaw({});
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->empty());
}

TEST(DecompressorTest, InflateRawRejectsInvalidData) {
  const std::array<uint8_t, 4> invalid = {0x01, 0x02, 0x03, 0x04};
  EXPECT_FALSE(InflateRaw(invalid).has_value());
}

TEST(DecompressorTest, InflateRawDecompressesValidPayload) {
  const std::string payload = "solidworks-stream";
  const std::vector<uint8_t> compressed =
      test::DeflateRaw(std::span<const uint8_t>(
          reinterpret_cast<const uint8_t*>(payload.data()), payload.size()));

  const auto result = InflateRaw(compressed);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(std::string(result->begin(), result->end()), payload);
}

TEST(DecompressorTest, InflateZlbRejectsTruncatedHeader) {
  const std::array<uint8_t, 12> short_block = {0};
  EXPECT_FALSE(InflateZlb(short_block).has_value());
}

TEST(DecompressorTest, InflateZlbRejectsInvalidCompressedLength) {
  std::vector<uint8_t> block(24, 0);
  block[16] = 0x04;
  block[20] = 0x20;
  EXPECT_FALSE(InflateZlb(block).has_value());
}

TEST(DecompressorTest, InflateZlbDecompressesValidBlock) {
  const std::string payload = "zlb payload";
  const std::vector<uint8_t> block = test::MakeZlbBlock(
      std::span<const uint8_t>(
          reinterpret_cast<const uint8_t*>(payload.data()), payload.size()));

  const auto result = InflateZlb(block);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(std::string(result->begin(), result->end()), payload);
}

}  // namespace
}  // namespace openswx::internal
