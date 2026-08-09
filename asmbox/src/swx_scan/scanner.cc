// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#include "swx_scan/scanner.h"
#include "openswx/version.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <system_error>

namespace fs = std::filesystem;

namespace sw_dumper::scan {

namespace {

std::string ToLowerCopy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return value;
}

bool IsSupportedExtension(const fs::path& path) {
  const std::string extension = ToLowerCopy(path.extension().string());
  return extension == ".sldprt" ||
         extension == ".sldasm" ||
         extension == ".slddrw";
}

std::string DocumentTypeToKey(openswx::DocumentType type) {
  switch (type) {
    case openswx::DocumentType::kPart:
      return "part";
    case openswx::DocumentType::kAssembly:
      return "assembly";
    case openswx::DocumentType::kDrawing:
      return "drawing";
  }
  return "part";
}

std::string VersionToKey(int version) {
  if (version <= 0) return "unknown";
  return std::to_string(version);
}

}  // namespace

ScanFileResult ScanFileResult::Success(openswx::DocumentType type, int version) {
  ScanFileResult result;
  result.ok = true;
  result.type = type;
  result.version = version;
  return result;
}

ScanFileResult ScanFileResult::Failure(std::string error_category) {
  ScanFileResult result;
  result.ok = false;
  result.error_category = std::move(error_category);
  return result;
}

std::string DetectPlatformString() {
  std::string os = "unknown";
#if defined(_WIN32)
  os = "windows";
#elif defined(__APPLE__)
  os = "macos";
#elif defined(__linux__)
  os = "linux";
#endif

  std::string arch = "unknown";
#if defined(__x86_64__) || defined(_M_X64)
  arch = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  arch = "arm64";
#elif defined(__i386__) || defined(_M_IX86)
  arch = "x86";
#endif

  return os + "-" + arch;
}

std::string CategorizeOpenError(std::string_view error) {
  if (error.starts_with("Cannot open file:")) {
    return "io_open_error";
  }
  if (error.starts_with("File is empty:")) {
    return "empty_file_error";
  }
  if (error.starts_with("Read error:")) {
    return "io_read_error";
  }
  if (error.find("pre-2015 format (OLE2/CFB) is not supported") !=
      std::string_view::npos) {
    return "unsupported_ole2_format";
  }
  if (error.find("ZIP/OPC (3DExperience) format is not yet supported") !=
      std::string_view::npos) {
    return "unsupported_zip_opc_format";
  }
  if (error.find("Unrecognised file format") != std::string_view::npos) {
    return "unrecognized_format";
  }
  if (error.find("Failed to parse modern SW format") !=
      std::string_view::npos) {
    return "parse_error";
  }
  return "unknown_error";
}

ScanFileResult ParseWithOpenswx(const fs::path& path) {
  const auto result = openswx::SwxDocument::Open(path);
  if (!result.ok()) {
    return ScanFileResult::Failure(CategorizeOpenError(result.error()));
  }
  return ScanFileResult::Success(result.value().type(), result.value().version());
}

nlohmann::json ScanReportToJson(const ScanReport& report) {
  return nlohmann::json{
      {"openswx_version", report.openswx_version},
      {"platform", report.platform},
      {"files", report.files},
      {"success", report.success},
      {"failed", report.failed},
      {"types", report.types},
      {"versions", report.versions},
      {"errors", report.errors},
  };
}

std::string FormatConsoleReport(const ScanReport& report) {
  std::ostringstream out;
  out << "openswx compatibility scan\n\n";
  out << "Files found:       " << report.files << "\n";
  out << "Successfully read: " << report.success << "\n";
  out << "Failed:            " << report.failed << "\n\n";

  const auto type_value = [&](std::string_view key) -> std::uint64_t {
    const auto it = report.types.find(std::string(key));
    return it == report.types.end() ? 0 : it->second;
  };

  out << "SLDPRT: " << type_value("part") << "\n";
  out << "SLDASM: " << type_value("assembly") << "\n";
  out << "SLDDRW: " << type_value("drawing") << "\n";

  if (!report.versions.empty()) {
    out << "\nVersions:\n";
    for (const auto& [version, count] : report.versions) {
      out << "  " << version << ": " << count << "\n";
    }
  }

  if (!report.errors.empty()) {
    out << "\nErrors:\n";
    for (const auto& [category, count] : report.errors) {
      out << "  " << category << ": " << count << "\n";
    }
  }

  return out.str();
}

openswx::Result<ScanReport> ScanDirectory(
    const fs::path& root, const ParseDocumentFn& parse_document) {
  if (parse_document == nullptr) {
    return openswx::Result<ScanReport>::Err("Parser callback is required");
  }

  std::error_code root_error;
  if (!fs::exists(root, root_error)) {
    return openswx::Result<ScanReport>::Err(
        "Scan root does not exist: " + root.string());
  }
  if (root_error) {
    return openswx::Result<ScanReport>::Err(
        "Cannot inspect scan root: " + root.string());
  }
  if (!fs::is_directory(root, root_error) || root_error) {
    return openswx::Result<ScanReport>::Err(
        "Scan root is not a directory: " + root.string());
  }

  ScanReport report;
  report.openswx_version = OPENSWX_VERSION_STRING;
  report.platform = DetectPlatformString();
  report.types.emplace("part", 0);
  report.types.emplace("assembly", 0);
  report.types.emplace("drawing", 0);

  fs::recursive_directory_iterator it(
      root, fs::directory_options::skip_permission_denied, root_error);
  if (root_error) {
    return openswx::Result<ScanReport>::Err(
        "Cannot enumerate scan root: " + root.string());
  }

  const fs::recursive_directory_iterator end;
  while (it != end) {
    if (it->is_regular_file(root_error) && !root_error &&
        IsSupportedExtension(it->path())) {
      ++report.files;
      const ScanFileResult file_result = parse_document(it->path());
      if (file_result.ok) {
        ++report.success;
        ++report.types[DocumentTypeToKey(file_result.type)];
        ++report.versions[VersionToKey(file_result.version)];
      } else {
        ++report.failed;
        ++report.errors[file_result.error_category];
      }
    }

    it.increment(root_error);
    if (root_error) {
      return openswx::Result<ScanReport>::Err(
          "Directory traversal failed under scan root: " + root.string());
    }
  }

  return openswx::Result<ScanReport>::Ok(std::move(report));
}

}  // namespace sw_dumper::scan
