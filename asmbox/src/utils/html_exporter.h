// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>

namespace sw_dumper::utils {

class HtmlExporter {
 public:
  // Writes a self-contained HTML report to output_path.
  // Returns true on success.
  [[nodiscard]] static bool Export(const std::filesystem::path& output_path,
                                   const nlohmann::json& data);
};

}  // namespace sw_dumper::utils
