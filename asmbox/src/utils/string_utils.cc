// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#include "utils/string_utils.h"

#include <algorithm>
#include <cstdio>

namespace sw_dumper::utils {

bool EndsWithCI(const std::string& str, const std::string& suffix) {
  if (suffix.size() > str.size()) return false;
  return std::equal(suffix.rbegin(), suffix.rend(), str.rbegin(),
                    [](char a, char b) {
                      return std::tolower(static_cast<unsigned char>(a)) ==
                             std::tolower(static_cast<unsigned char>(b));
                    });
}

std::string UrlDecode(const std::string& str) {
  std::string out;
  out.reserve(str.size());
  for (std::size_t i = 0; i < str.size(); ++i) {
    if (str[i] == '%' && i + 2 < str.size()) {
      unsigned int value = 0;
      std::sscanf(str.c_str() + i + 1, "%2x", &value);
      out += static_cast<char>(value);
      i += 2;
    } else if (str[i] == '+') {
      out += ' ';
    } else {
      out += str[i];
    }
  }
  return out;
}

std::string Base64UrlDecode(const std::string& input) {
  // Convert URL-safe alphabet to standard Base64 and add padding.
  std::string in = input;
  for (char& c : in) {
    if (c == '-') c = '+';
    if (c == '_') c = '/';
  }
  while (in.size() % 4) in += '=';

  // Decode.
  static constexpr int kInvalid = -1;
  static constexpr char kAlpha[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int table[256];
  std::fill(std::begin(table), std::end(table), kInvalid);
  for (int i = 0; i < 64; ++i)
    table[static_cast<unsigned char>(kAlpha[i])] = i;

  std::string out;
  int val = 0, valb = -8;
  for (unsigned char c : in) {
    if (table[c] == kInvalid) break;
    val = (val << 6) + table[c];
    valb += 6;
    if (valb >= 0) {
      out.push_back(static_cast<char>((val >> valb) & 0xFF));
      valb -= 8;
    }
  }
  return out;
}

// ── UTF-8 helpers ─────────────────────────────────────────────────────────────

// Returns the expected continuation bytes for a leading byte, or -1 if invalid.
static int Utf8SequenceLen(unsigned char c) {
  if (c < 0x80) return 1;        // single-byte ASCII
  if (c < 0xC0) return -1;       // bare continuation byte — invalid leader
  if (c < 0xE0) return 2;
  if (c < 0xF0) return 3;
  if (c < 0xF8) return 4;
  return -1;
}

std::string SanitizeUtf8(const std::string& str) {
  std::string out;
  out.reserve(str.size());

  for (std::size_t i = 0; i < str.size(); ) {
    unsigned char lead = static_cast<unsigned char>(str[i]);
    int seq_len = Utf8SequenceLen(lead);

    if (seq_len == 1) {
      // Pure ASCII — always valid.
      out += str[i++];
      continue;
    }

    if (seq_len < 0) {
      // Invalid leader byte.  Treat as Latin-1 and re-encode as UTF-8.
      out += static_cast<char>(0xC0 | (lead >> 6));
      out += static_cast<char>(0x80 | (lead & 0x3F));
      ++i;
      continue;
    }

    // Check that we have enough bytes and all are valid continuations.
    bool valid = (i + static_cast<std::size_t>(seq_len) <= str.size());
    for (int k = 1; valid && k < seq_len; ++k) {
      unsigned char cont = static_cast<unsigned char>(str[i + k]);
      if ((cont & 0xC0) != 0x80) valid = false;
    }

    if (valid) {
      // Copy the whole sequence as-is.
      out.append(str, i, static_cast<std::size_t>(seq_len));
      i += static_cast<std::size_t>(seq_len);
    } else {
      // Broken sequence — encode the lead byte as Latin-1.
      out += static_cast<char>(0xC0 | (lead >> 6));
      out += static_cast<char>(0x80 | (lead & 0x3F));
      ++i;
    }
  }

  return out;
}

}  // namespace sw_dumper::utils
