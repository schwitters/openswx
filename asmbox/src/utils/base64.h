// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#ifndef SW_DUMPER_UTILS_BASE64_H_
#define SW_DUMPER_UTILS_BASE64_H_

#include <span>  // C++20
#include <string>
#include <vector>

namespace sw_dumper::utils {

// Einfacher, robuster Base64 Encoder für C++20
inline std::string Base64Encode(std::span<const uint8_t> data) {
  static constexpr char kEncodeTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  std::string output;
  output.reserve(((data.size() + 2) / 3) * 4);

  size_t i = 0;
  for (; i + 2 < data.size(); i += 3) {
    uint32_t triple = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
    output.push_back(kEncodeTable[(triple >> 18) & 0x3F]);
    output.push_back(kEncodeTable[(triple >> 12) & 0x3F]);
    output.push_back(kEncodeTable[(triple >> 6) & 0x3F]);
    output.push_back(kEncodeTable[triple & 0x3F]);
  }

  if (i < data.size()) {
    uint32_t triple =
        (data[i] << 16) | ((i + 1 < data.size() ? data[i + 1] : 0) << 8);
    output.push_back(kEncodeTable[(triple >> 18) & 0x3F]);
    output.push_back(kEncodeTable[(triple >> 12) & 0x3F]);

    if (i + 1 < data.size()) {
      output.push_back(kEncodeTable[(triple >> 6) & 0x3F]);
    } else {
      output.push_back('=');
    }
    output.push_back('=');
  }

  return output;
}

}  // namespace sw_dumper::utils

#endif  // SW_DUMPER_UTILS_BASE64_H_
