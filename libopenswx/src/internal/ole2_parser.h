// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#pragma once

// internal/ole2_parser.h — Minimal OLE2 Compound Document parser.
//
// Parses legacy SolidWorks files (pre-2015) stored in OLE2/CFB format
// (magic bytes D0 CF 11 E0).  Streams whose names end in "__Zip" are
// raw-deflate decompressed; the suffix is stripped from the key.
// See format-spec.md §4 and Microsoft [MS-CFB] for format details.

#include <cstdint>
#include <span>

#include "internal/stream_store.h"

namespace openswx::internal {

// Returns true iff data begins with the OLE2 magic D0 CF 11 E0 A1 B1 1A E1.
[[nodiscard]] bool IsOle2(std::span<const uint8_t> data) noexcept;

// Parses an OLE2 file and populates streams with all readable streams.
// Returns false if the file header is malformed.
[[nodiscard]] bool ParseOle2Format(std::span<const uint8_t> data,
                                    StreamMap& streams);

}  // namespace openswx::internal
