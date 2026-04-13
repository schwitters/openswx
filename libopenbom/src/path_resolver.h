// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "openbom/transformer.h"

namespace openbom::internal {

/// Resolves a Windows absolute path stored inside a SolidWorks file to a
/// path that is accessible on the host filesystem.
///
/// Resolution order:
///  1. Apply each substitution rule in order.  For a matching prefix, build
///     the candidate path and test whether it exists (exact first, then
///     case-insensitive walk if enabled).
///  2. If no substitution succeeds, search for the filename in each
///     search_dir (shallow by default, recursive when
///     PathResolverConfig::recursive_search is set).
class PathResolver {
 public:
  explicit PathResolver(const PathResolverConfig& cfg);

  /// Resolve @p windows_path to a host path.
  /// Returns nullopt when no match is found.
  [[nodiscard]] std::optional<std::filesystem::path> Resolve(
      const std::string& windows_path) const;

 private:
  const PathResolverConfig& cfg_;

  // Converts backslashes to forward slashes.
  static std::string NormalizeSeparators(const std::string& p);

  // Returns p if it exists, nullopt otherwise.
  static std::optional<std::filesystem::path> TryExact(
      const std::filesystem::path& p);

  // Walks p's parent directories case-insensitively component by component.
  // Returns the resolved path if every component is found, nullopt otherwise.
  static std::optional<std::filesystem::path> TryCaseInsensitive(
      const std::filesystem::path& p);

  // Looks for p.filename() in each cfg_.search_dirs entry.
  [[nodiscard]] std::optional<std::filesystem::path> SearchByFilename(
      const std::filesystem::path& p) const;
};

}  // namespace openbom::internal
