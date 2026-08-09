// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "openswx/document.h"
#include "openswx/types.h"

namespace sw_dumper::scan {

struct ScanFileResult {
  [[nodiscard]] static ScanFileResult Success(openswx::DocumentType type,
                                              int version);
  [[nodiscard]] static ScanFileResult Failure(std::string error_category);

  bool ok = false;
  openswx::DocumentType type = openswx::DocumentType::kPart;
  int version = 0;
  std::string error_category;
};

struct ScanReport {
  std::string openswx_version;
  std::string platform;
  std::uint64_t files = 0;
  std::uint64_t success = 0;
  std::uint64_t failed = 0;
  std::map<std::string, std::uint64_t> types;
  std::map<std::string, std::uint64_t> versions;
  std::map<std::string, std::uint64_t> errors;
};

using ParseDocumentFn =
    std::function<ScanFileResult(const std::filesystem::path&)>;

[[nodiscard]] openswx::Result<ScanReport> ScanDirectory(
    const std::filesystem::path& root, const ParseDocumentFn& parse_document);

[[nodiscard]] ScanFileResult ParseWithOpenswx(
    const std::filesystem::path& path);

[[nodiscard]] nlohmann::json ScanReportToJson(const ScanReport& report);
[[nodiscard]] std::string FormatConsoleReport(const ScanReport& report);
[[nodiscard]] std::string DetectPlatformString();
[[nodiscard]] std::string CategorizeOpenError(std::string_view error);

}  // namespace sw_dumper::scan
