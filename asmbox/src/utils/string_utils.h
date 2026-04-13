// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#pragma once

#include <string>

namespace sw_dumper::utils {

// Case-insensitive suffix check (ASCII only).
bool EndsWithCI(const std::string& str, const std::string& suffix);

// Percent-decodes a URL-encoded string.
std::string UrlDecode(const std::string& str);

// Decodes a Base64url-encoded string (URL-safe alphabet, no padding required).
std::string Base64UrlDecode(const std::string& input);

// Returns str if it is valid UTF-8.  If the string contains invalid UTF-8
// byte sequences (e.g. stray Windows-1252 bytes), it converts those bytes as
// Latin-1 and re-encodes them as UTF-8, so that the output is always valid.
std::string SanitizeUtf8(const std::string& str);

}  // namespace sw_dumper::utils
