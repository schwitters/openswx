// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#pragma once

// internal/sheet_name_reader.h — Parser for the SheetPreviews/SheetNames
// binary stream in SolidWorks drawing files.
//
// Format (see format-spec.md §5.12):
//   Offset 0  : uint16 LE — number of sheets
//   Per sheet:
//     +0  FF FE   (per-entry marker)
//     +2  FF      (string type marker)
//     +3  uint8   — number of UTF-16LE code units
//     +4  n*2     — UTF-16LE string data (no null terminator)

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace openswx::internal {

// Parses the SheetPreviews/SheetNames stream and returns sheet names as UTF-8.
// Returns an empty vector if the stream is absent or malformed.
[[nodiscard]] std::vector<std::string> ParseSheetNames(
    std::span<const uint8_t> data);

}  // namespace openswx::internal
