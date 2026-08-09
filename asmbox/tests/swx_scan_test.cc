// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "swx_scan/scanner.h"

namespace fs = std::filesystem;

namespace sw_dumper::scan {
namespace {

class TempDir {
 public:
  TempDir() {
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = fs::temp_directory_path() /
            ("swx_scan_test_" + std::to_string(nonce));
    std::error_code error;
    fs::create_directories(path_, error);
  }

  ~TempDir() {
    std::error_code error;
    fs::remove_all(path_, error);
  }

  [[nodiscard]] const fs::path& path() const noexcept { return path_; }

 private:
  fs::path path_;
};

void WriteFile(const fs::path& path, std::string_view contents = "dummy") {
  std::error_code error;
  fs::create_directories(path.parent_path(), error);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << contents;
}

ScanFileResult FakeParser(const fs::path& path) {
  std::string filename = path.filename().string();
  for (char& c : filename) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (filename == "part.SLDPRT") {
    return ScanFileResult::Success(openswx::DocumentType::kPart, 12155);
  }
  if (filename == "part.sldprt") {
    return ScanFileResult::Success(openswx::DocumentType::kPart, 12155);
  }
  if (filename == "asm.sldasm") {
    return ScanFileResult::Success(openswx::DocumentType::kAssembly, 11142);
  }
  if (filename == "drawing.slddrw") {
    return ScanFileResult::Success(openswx::DocumentType::kDrawing, 12155);
  }
  if (filename == "broken.sldprt") {
    return ScanFileResult::Failure("parse_error");
  }
  return ScanFileResult::Failure("unknown_error");
}

TEST(SwxScanTest, ScansEmptyDirectory) {
  TempDir temp_dir;
  const auto result = ScanDirectory(temp_dir.path(), FakeParser);
  ASSERT_TRUE(result.ok()) << result.error();
  EXPECT_EQ(result.value().files, 0u);
  EXPECT_EQ(result.value().success, 0u);
  EXPECT_EQ(result.value().failed, 0u);
}

TEST(SwxScanTest, IgnoresUnrelatedFiles) {
  TempDir temp_dir;
  WriteFile(temp_dir.path() / "notes.txt");
  WriteFile(temp_dir.path() / "model.step");

  const auto result = ScanDirectory(temp_dir.path(), FakeParser);
  ASSERT_TRUE(result.ok()) << result.error();
  EXPECT_EQ(result.value().files, 0u);
}

TEST(SwxScanTest, ScansOneSupportedFile) {
  TempDir temp_dir;
  WriteFile(temp_dir.path() / "part.SLDPRT");

  const auto result = ScanDirectory(temp_dir.path(), FakeParser);
  ASSERT_TRUE(result.ok()) << result.error();
  EXPECT_EQ(result.value().files, 1u);
  EXPECT_EQ(result.value().success, 1u);
  EXPECT_EQ(result.value().failed, 0u);
  EXPECT_EQ(result.value().types.at("part"), 1u);
  EXPECT_EQ(result.value().versions.at("12155"), 1u);
}

TEST(SwxScanTest, SupportsRecursiveDirectoriesAndCaseInsensitiveExtensions) {
  TempDir temp_dir;
  WriteFile(temp_dir.path() / "nested/asm.SLDASM");
  WriteFile(temp_dir.path() / "nested/deeper/drawing.slddrw");

  const auto result = ScanDirectory(temp_dir.path(), FakeParser);
  ASSERT_TRUE(result.ok()) << result.error();
  EXPECT_EQ(result.value().files, 2u);
  EXPECT_EQ(result.value().success, 2u);
  EXPECT_EQ(result.value().types.at("assembly"), 1u);
  EXPECT_EQ(result.value().types.at("drawing"), 1u);
}

TEST(SwxScanTest, AggregatesMixedSuccessAndFailure) {
  TempDir temp_dir;
  WriteFile(temp_dir.path() / "part.SLDPRT");
  WriteFile(temp_dir.path() / "broken.SLDPRT");

  const auto result = ScanDirectory(temp_dir.path(), FakeParser);
  ASSERT_TRUE(result.ok()) << result.error();
  EXPECT_EQ(result.value().files, 2u);
  EXPECT_EQ(result.value().success, 1u);
  EXPECT_EQ(result.value().failed, 1u);
  EXPECT_EQ(result.value().errors.at("parse_error"), 1u);
}

TEST(SwxScanTest, JsonReportContainsOnlyAggregateInformation) {
  TempDir temp_dir;
  WriteFile(temp_dir.path() / "secret_project/part.SLDPRT");
  WriteFile(temp_dir.path() / "secret_project/broken.SLDPRT");

  const auto result = ScanDirectory(temp_dir.path(), FakeParser);
  ASSERT_TRUE(result.ok()) << result.error();

  const std::string json = ScanReportToJson(result.value()).dump();
  EXPECT_EQ(json.find("part.SLDPRT"), std::string::npos);
  EXPECT_EQ(json.find("broken.SLDPRT"), std::string::npos);
  EXPECT_EQ(json.find("secret_project"), std::string::npos);
  EXPECT_NE(json.find("\"files\":2"), std::string::npos);
  EXPECT_NE(json.find("\"parse_error\":1"), std::string::npos);
}

TEST(SwxScanTest, ConsoleReportContainsNoPathsOrFilenames) {
  TempDir temp_dir;
  WriteFile(temp_dir.path() / "customer_a/part.SLDPRT");

  const auto result = ScanDirectory(temp_dir.path(), FakeParser);
  ASSERT_TRUE(result.ok()) << result.error();

  const std::string report = FormatConsoleReport(result.value());
  EXPECT_EQ(report.find("customer_a"), std::string::npos);
  EXPECT_EQ(report.find("part.SLDPRT"), std::string::npos);
  EXPECT_NE(report.find("Files found:"), std::string::npos);
}

TEST(SwxScanTest, CategorizesKnownParserErrors) {
  EXPECT_EQ(CategorizeOpenError("Cannot open file: /tmp/x"),
            "io_open_error");
  EXPECT_EQ(CategorizeOpenError("File is empty: /tmp/x"),
            "empty_file_error");
  EXPECT_EQ(CategorizeOpenError(
                "SolidWorks pre-2015 format (OLE2/CFB) is not supported."),
            "unsupported_ole2_format");
  EXPECT_EQ(CategorizeOpenError(
                "ZIP/OPC (3DExperience) format is not yet supported"),
            "unsupported_zip_opc_format");
}

}  // namespace
}  // namespace sw_dumper::scan
