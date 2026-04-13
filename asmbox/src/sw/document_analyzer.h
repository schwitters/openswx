// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>

namespace sw_dumper::sw {

// Analyzes a SolidWorks file using libopenswx and serializes the result into
// a nlohmann::json object whose schema matches the asmbox HTML viewer.
class DocumentAnalyzer {
 public:
  // Fills *data with the document representation.  Returns true on success.
  [[nodiscard]] bool AnalyzeFile(const std::filesystem::path& path,
                                  nlohmann::json* data) const;
};

}  // namespace sw_dumper::sw
