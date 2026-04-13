// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#pragma once

// internal/stream_store.h — Type alias for the decoded stream map.
//
// Both the modern chunk parser and the OLE2 parser populate a StreamMap:
// a flat mapping from stream name (with '/' as path separator) to its
// decoded, decompressed byte content.
//
// Example stream names:
//   "docProps/custom.xml"
//   "Contents/Config-0-Partition"
//   "swXmlContents/COMPINSTANCETREE"
//   "PreviewPNG"

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace openswx::internal {

using StreamMap = std::unordered_map<std::string, std::vector<uint8_t>>;

// Returns a span over the contents of the named stream, or an empty span
// if the stream is not present.
[[nodiscard]] inline std::span<const uint8_t> GetStream(
    const StreamMap& store, const std::string& name) noexcept {
  auto it = store.find(name);
  if (it == store.end()) return {};
  return std::span<const uint8_t>(it->second);
}

// Returns true iff the StreamMap contains any key starting with prefix.
[[nodiscard]] inline bool HasStreamWithPrefix(const StreamMap& store,
                                               std::string_view prefix) {
  for (const auto& [key, _] : store) {
    if (key.starts_with(prefix)) return true;
  }
  return false;
}

}  // namespace openswx::internal
