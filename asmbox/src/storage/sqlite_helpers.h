// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#pragma once
#include <sqlite3.h>
#include <string>
#include "utils/string_utils.h"

namespace sw_dumper::storage {

// Liest Text aus SQLite sicher aus (handhabt NULL und Encoding)
inline std::string SafeColumnText(sqlite3_stmt* stmt, int col) {
  const char* text = (const char*)sqlite3_column_text(stmt, col);
  if (!text)
    return "";
  return sw_dumper::utils::SanitizeUtf8(text);
}

}  // namespace sw_dumper::storage
