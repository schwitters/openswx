// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

/// @file json_writer.h
/// @brief Serialises a @ref openbom::Bom to JSON.
///
/// The output format is a single JSON object with two top-level keys:
///  - `"bom"`: the root @ref openbom::BomItem tree.
///  - `"warnings"`: array of human-readable warning strings accumulated
///    during BOM traversal.
///
/// Each BOM item is a JSON object with the following fields:
/// | Field             | Type             | Notes                          |
/// |-------------------|------------------|--------------------------------|
/// | `name`            | string           |                                |
/// | `type`            | string           | `"part"` / `"assembly"` / ...  |
/// | `quantity`        | integer          |                                |
/// | `configuration`   | string           |                                |
/// | `file_path`       | string           | Empty when unresolved.         |
/// | `windows_path`    | string           | Original SW path.              |
/// | `is_suppressed`   | bool             |                                |
/// | `exclude_from_bom`| bool             |                                |
/// | `properties`      | object           | Key/value custom properties.   |
/// | `children`        | array of items   | Recursive sub-BOM.             |

#pragma once

#include <filesystem>
#include <string>

#include "openbom/types.h"

namespace openbom {

/// @brief Serialises a @ref Bom to JSON.
///
/// Construct once, call @ref Write() or @ref WriteToFile() as many times as
/// needed.  The writer is stateless and thread-safe.
class JsonWriter {
 public:
  /// @brief Constructs a writer.
  /// @param pretty When `true` (default) the output is indented with 2-space
  ///               indentation.  When `false` the output is compact.
  explicit JsonWriter(bool pretty = true) : pretty_(pretty) {}

  /// @brief Serialises @p bom to a JSON string.
  [[nodiscard]] std::string Write(const Bom& bom) const;

  /// @brief Writes the JSON representation of @p bom to @p path.
  ///
  /// Creates or truncates the file at @p path.
  ///
  /// @return `true` on success, `false` if the file could not be written.
  [[nodiscard]] bool WriteToFile(const Bom& bom,
                                 const std::filesystem::path& path) const;

 private:
  bool pretty_;
};

}  // namespace openbom
