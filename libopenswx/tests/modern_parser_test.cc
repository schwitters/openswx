// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "internal/modern_parser.h"
#include "internal/stream_store.h"
#include "test_helpers.h"

namespace openswx::internal {
namespace {

TEST(ModernParserTest, RejectsTooShortFile) {
  const std::array<uint8_t, 7> file = {0};
  StreamMap streams;
  EXPECT_FALSE(ParseModernFormat(file, streams));
  EXPECT_TRUE(streams.empty());
}

TEST(ModernParserTest, ParsesValidInlineChunk) {
  const std::string payload = "preview-png";
  const std::vector<uint8_t> file = test::MakeModernChunkFile(
      "PreviewPNG",
      std::span<const uint8_t>(
          reinterpret_cast<const uint8_t*>(payload.data()), payload.size()));

  StreamMap streams;
  ASSERT_TRUE(ParseModernFormat(file, streams));
  ASSERT_EQ(streams.size(), 1u);
  EXPECT_EQ(std::string(streams.at("PreviewPNG").begin(),
                        streams.at("PreviewPNG").end()),
            payload);
}

TEST(ModernParserTest, IgnoresChunkWithOversizedNameLength) {
  const std::array<uint8_t, 1> payload = {0x42};
  std::vector<uint8_t> file =
      test::MakeModernChunkFile("A", payload);
  const std::size_t name_size_offset = 8 + 0x1A;
  file[name_size_offset] = 0x01;
  file[name_size_offset + 1] = 0x02;
  file[name_size_offset + 2] = 0x00;
  file[name_size_offset + 3] = 0x00;

  StreamMap streams;
  EXPECT_TRUE(ParseModernFormat(file, streams));
  EXPECT_TRUE(streams.empty());
}

TEST(ModernParserTest, IgnoresChunkWithTruncatedPayload) {
  const std::array<uint8_t, 1> payload = {0x42};
  std::vector<uint8_t> file =
      test::MakeModernChunkFile("PreviewPNG", payload);
  file.pop_back();

  StreamMap streams;
  EXPECT_TRUE(ParseModernFormat(file, streams));
  EXPECT_TRUE(streams.empty());
}

TEST(ModernParserTest, IgnoresChunkWithOversizedCompressedSize) {
  const std::array<uint8_t, 1> payload = {0x42};
  std::vector<uint8_t> file =
      test::MakeModernChunkFile("PreviewPNG", payload);
  const std::size_t compressed_size_offset = 8 + 0x12;
  file[compressed_size_offset] = 0x00;
  file[compressed_size_offset + 1] = 0x00;
  file[compressed_size_offset + 2] = 0x00;
  file[compressed_size_offset + 3] = 0x08;

  StreamMap streams;
  EXPECT_TRUE(ParseModernFormat(file, streams));
  EXPECT_TRUE(streams.empty());
}

TEST(ModernParserTest, IgnoresChunkWhenDecompressionFails) {
  const std::array<uint8_t, 1> payload = {0x42};
  std::vector<uint8_t> file =
      test::MakeModernChunkFile("PreviewPNG", payload);
  file.back() ^= 0xFFu;

  StreamMap streams;
  EXPECT_TRUE(ParseModernFormat(file, streams));
  EXPECT_TRUE(streams.empty());
}

TEST(ModernParserTest, KeepsFirstStreamForDuplicateNames) {
  const std::string first_payload = "first";
  const std::string second_payload = "second";
  std::vector<uint8_t> file = test::MakeModernChunkFile(
      "PreviewPNG",
      std::span<const uint8_t>(
          reinterpret_cast<const uint8_t*>(first_payload.data()),
          first_payload.size()));
  std::vector<uint8_t> second_chunk = test::MakeModernChunkFile(
      "PreviewPNG",
      std::span<const uint8_t>(
          reinterpret_cast<const uint8_t*>(second_payload.data()),
          second_payload.size()));
  file.insert(file.end(), second_chunk.begin() + 8, second_chunk.end());

  StreamMap streams;
  ASSERT_TRUE(ParseModernFormat(file, streams));
  ASSERT_EQ(streams.size(), 1u);
  EXPECT_EQ(std::string(streams.at("PreviewPNG").begin(),
                        streams.at("PreviewPNG").end()),
            first_payload);
}

TEST(ModernParserTest, AcceptsUnknownSectionTypeForInlineData) {
  const std::string payload = "unknown-section";
  const std::vector<uint8_t> file = test::MakeModernChunkFile(
      "PreviewPNG",
      std::span<const uint8_t>(
          reinterpret_cast<const uint8_t*>(payload.data()), payload.size()),
      0x77);

  StreamMap streams;
  ASSERT_TRUE(ParseModernFormat(file, streams));
  ASSERT_EQ(streams.size(), 1u);
  EXPECT_EQ(std::string(streams.at("PreviewPNG").begin(),
                        streams.at("PreviewPNG").end()),
            payload);
}

}  // namespace
}  // namespace openswx::internal
