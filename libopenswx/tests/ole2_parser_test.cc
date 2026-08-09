// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "internal/ole2_parser.h"

namespace openswx::internal {
namespace {

TEST(Ole2ParserTest, DetectsOle2Magic) {
  const std::array<uint8_t, 8> file = {
      0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
  EXPECT_TRUE(IsOle2(file));
}

TEST(Ole2ParserTest, RejectsTruncatedHeader) {
  const std::array<uint8_t, 32> file = {0};
  StreamMap streams;
  EXPECT_FALSE(ParseOle2Format(file, streams));
  EXPECT_TRUE(streams.empty());
}

}  // namespace
}  // namespace openswx::internal
