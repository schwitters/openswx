// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#include <gtest/gtest.h>

#include "utils/string_utils.h"

namespace sw_dumper::utils {

// ── EndsWithCI ────────────────────────────────────────────────────────────────

TEST(EndsWithCITest, MatchesExact) {
  EXPECT_TRUE(EndsWithCI("file.SLDPRT", ".SLDPRT"));
}

TEST(EndsWithCITest, CaseInsensitive) {
  EXPECT_TRUE(EndsWithCI("file.sldprt", ".SLDPRT"));
  EXPECT_TRUE(EndsWithCI("file.SLDPRT", ".sldprt"));
  EXPECT_TRUE(EndsWithCI("file.SlDpRt", ".sLdPrT"));
}

TEST(EndsWithCITest, NoMatch) {
  EXPECT_FALSE(EndsWithCI("file.SLDASM", ".SLDPRT"));
  EXPECT_FALSE(EndsWithCI("short", ".longersuffix"));
}

TEST(EndsWithCITest, EmptySuffix) {
  EXPECT_TRUE(EndsWithCI("anything", ""));
}

// ── UrlDecode ────────────────────────────────────────────────────────────────

TEST(UrlDecodeTest, NoEncoding) {
  EXPECT_EQ(UrlDecode("hello"), "hello");
}

TEST(UrlDecodeTest, PlusAsSpace) {
  EXPECT_EQ(UrlDecode("hello+world"), "hello world");
}

TEST(UrlDecodeTest, PercentEncoded) {
  EXPECT_EQ(UrlDecode("M%C3%BCnchen"), "München");
}

TEST(UrlDecodeTest, MixedEncoding) {
  EXPECT_EQ(UrlDecode("a%20b+c"), "a b c");
}

// ── Base64UrlDecode ───────────────────────────────────────────────────────────

TEST(Base64UrlDecodeTest, StandardBase64) {
  // "hello" = aGVsbG8=
  EXPECT_EQ(Base64UrlDecode("aGVsbG8="), "hello");
}

TEST(Base64UrlDecodeTest, UrlSafeAlphabet) {
  // Use '-' instead of '+' and '_' instead of '/'
  // "hello world" = aGVsbG8gd29ybGQ=  (standard)
  EXPECT_EQ(Base64UrlDecode("aGVsbG8gd29ybGQ="), "hello world");
}

TEST(Base64UrlDecodeTest, NoPaddingRequired) {
  // Decoder must add padding automatically.
  EXPECT_EQ(Base64UrlDecode("aGVsbG8"), "hello");
}

TEST(Base64UrlDecodeTest, EmptyInput) {
  EXPECT_EQ(Base64UrlDecode(""), "");
}

// ── SanitizeUtf8 ──────────────────────────────────────────────────────────────

TEST(SanitizeUtf8Test, ValidAsciiPassThrough) {
  std::string s = "Hello World 123";
  EXPECT_EQ(SanitizeUtf8(s), s);
}

TEST(SanitizeUtf8Test, ValidUtf8PassThrough) {
  std::string s = "München Straße";
  EXPECT_EQ(SanitizeUtf8(s), s);
}

TEST(SanitizeUtf8Test, InvalidByteGetsSanitized) {
  // 0x80 is a bare UTF-8 continuation byte — never valid as a leading byte.
  // Build the string explicitly to avoid C++ hex-escape digit absorption.
  std::string bad;
  bad += 'a';
  bad += static_cast<char>(0x80);
  bad += 'b';

  std::string result = SanitizeUtf8(bad);
  EXPECT_FALSE(result.empty());
  EXPECT_EQ(result.front(), 'a');
  EXPECT_EQ(result.back(), 'b');
  // The raw 0x80 byte must not survive as a bare byte at position 1.
  EXPECT_NE(static_cast<unsigned char>(result[1]), 0x80u);
}

TEST(SanitizeUtf8Test, EmptyStringPassThrough) {
  EXPECT_EQ(SanitizeUtf8(""), "");
}

}  // namespace sw_dumper::utils
