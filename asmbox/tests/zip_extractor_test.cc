// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#include <zip.h>

#include <sys/stat.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "utils/zip_extractor.h"

namespace fs = std::filesystem;

namespace sw_dumper::utils {
namespace {

struct ZipEntrySpec {
  std::string name;
  std::string contents;
  bool is_directory = false;
  bool set_unix_mode = false;
  mode_t unix_mode = 0;
};

class TempDir {
 public:
  TempDir() {
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = fs::temp_directory_path() /
            ("asmbox_zip_test_" + std::to_string(nonce));
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

std::string ReadFile(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::string BuildZipArchive(const fs::path& zip_path,
                            const std::vector<ZipEntrySpec>& entries) {
  int error_code = 0;
  zip_t* archive = zip_open(zip_path.string().c_str(),
                            ZIP_CREATE | ZIP_TRUNCATE, &error_code);
  EXPECT_NE(archive, nullptr);
  if (archive == nullptr) return {};

  for (const ZipEntrySpec& entry : entries) {
    zip_int64_t index = -1;
    if (entry.is_directory) {
      index = zip_dir_add(archive, entry.name.c_str(), ZIP_FL_ENC_UTF_8);
    } else {
      zip_source_t* source =
          zip_source_buffer(archive, entry.contents.data(), entry.contents.size(),
                            0);
      EXPECT_NE(source, nullptr);
      if (source == nullptr) {
        zip_close(archive);
        return {};
      }

      index = zip_file_add(archive, entry.name.c_str(), source,
                           ZIP_FL_ENC_UTF_8);
      EXPECT_GE(index, 0);
      if (index < 0) {
        zip_source_free(source);
        zip_close(archive);
        return {};
      }
    }

    EXPECT_GE(index, 0);
    if (index < 0) {
      zip_close(archive);
      return {};
    }

    if (entry.set_unix_mode) {
      const zip_uint32_t attributes =
          static_cast<zip_uint32_t>(entry.unix_mode) << 16;
      EXPECT_EQ(zip_file_set_external_attributes(
                    archive, static_cast<zip_uint64_t>(index),
                    ZIP_FL_ENC_UTF_8, ZIP_OPSYS_UNIX, attributes),
                0);
    }
  }

  EXPECT_EQ(zip_close(archive), 0);
  return ReadFile(zip_path);
}

bool CreateDirectorySymlink(const fs::path& target, const fs::path& link) {
  std::error_code error;
#if defined(_WIN32)
  fs::create_directory_symlink(target, link, error);
#else
  fs::create_directory_symlink(target, link, error);
#endif
  return !error;
}

TEST(ZipExtractorTest, ExtractsRegularFiles) {
  TempDir temp_dir;
  const fs::path zip_path = temp_dir.path() / "input.zip";
  const fs::path target_dir = temp_dir.path() / "workspace";

  const std::string zip_data = BuildZipArchive(zip_path, {
      {.name = "part.SLDPRT", .contents = "part-data"},
      {.name = "meta/info.txt", .contents = "hello"},
  });

  const ZipExtractionResult result = UnzipToFolder(zip_data, target_dir);
  ASSERT_TRUE(result.ok()) << result.error();
  EXPECT_EQ(ReadFile(target_dir / "part.SLDPRT"), "part-data");
  EXPECT_EQ(ReadFile(target_dir / "meta/info.txt"), "hello");
}

TEST(ZipExtractorTest, ExtractsNestedDirectories) {
  TempDir temp_dir;
  const fs::path zip_path = temp_dir.path() / "nested.zip";
  const fs::path target_dir = temp_dir.path() / "workspace";

  const std::string zip_data = BuildZipArchive(zip_path, {
      {.name = "nested/"},
      {.name = "nested/deeper/"},
      {.name = "nested/deeper/assy.SLDASM", .contents = "assembly"},
  });

  const ZipExtractionResult result = UnzipToFolder(zip_data, target_dir);
  ASSERT_TRUE(result.ok()) << result.error();
  EXPECT_EQ(ReadFile(target_dir / "nested/deeper/assy.SLDASM"), "assembly");
}

TEST(ZipExtractorTest, RejectsParentTraversal) {
  TempDir temp_dir;
  const fs::path zip_path = temp_dir.path() / "traversal.zip";
  const fs::path target_dir = temp_dir.path() / "workspace";
  const fs::path outside_file = temp_dir.path() / "outside.txt";

  const std::string zip_data = BuildZipArchive(zip_path, {
      {.name = "../outside.txt", .contents = "escape"},
  });

  const ZipExtractionResult result = UnzipToFolder(zip_data, target_dir);
  EXPECT_FALSE(result.ok());
  EXPECT_NE(result.error().find("traversal"), std::string::npos);
  EXPECT_FALSE(fs::exists(outside_file));
}

TEST(ZipExtractorTest, RejectsNestedParentTraversal) {
  TempDir temp_dir;
  const fs::path zip_path = temp_dir.path() / "nested_traversal.zip";
  const fs::path target_dir = temp_dir.path() / "workspace";
  const fs::path outside_file = temp_dir.path() / "outside";

  const std::string zip_data = BuildZipArchive(zip_path, {
      {.name = "foo/../../outside", .contents = "escape"},
  });

  const ZipExtractionResult result = UnzipToFolder(zip_data, target_dir);
  EXPECT_FALSE(result.ok());
  EXPECT_NE(result.error().find("traversal"), std::string::npos);
  EXPECT_FALSE(fs::exists(outside_file));
}

TEST(ZipExtractorTest, RejectsAbsoluteUnixPaths) {
  TempDir temp_dir;
  const fs::path zip_path = temp_dir.path() / "absolute.zip";
  const fs::path target_dir = temp_dir.path() / "workspace";
  const fs::path outside_file = temp_dir.path() / "absolute_target.txt";

  const std::string zip_data = BuildZipArchive(zip_path, {
      {.name = outside_file.string(), .contents = "escape"},
  });

  const ZipExtractionResult result = UnzipToFolder(zip_data, target_dir);
  EXPECT_FALSE(result.ok());
  EXPECT_NE(result.error().find("absolute path"), std::string::npos);
  EXPECT_FALSE(fs::exists(outside_file));
}

TEST(ZipExtractorTest, RejectsWindowsAbsolutePaths) {
  TempDir temp_dir;
  const fs::path zip_path = temp_dir.path() / "windows_absolute.zip";
  const fs::path target_dir = temp_dir.path() / "workspace";

  const std::string zip_data = BuildZipArchive(zip_path, {
      {.name = "C:/outside.txt", .contents = "escape"},
  });

  const ZipExtractionResult result = UnzipToFolder(zip_data, target_dir);
  EXPECT_FALSE(result.ok());
  EXPECT_NE(result.error().find("absolute path"), std::string::npos);
}

TEST(ZipExtractorTest, RejectsOversizedFiles) {
  TempDir temp_dir;
  const fs::path zip_path = temp_dir.path() / "oversized.zip";
  const fs::path target_dir = temp_dir.path() / "workspace";

  const std::string large_contents(32, 'A');
  const std::string zip_data = BuildZipArchive(zip_path, {
      {.name = "big.SLDPRT", .contents = large_contents},
  });

  ZipExtractionLimits limits;
  limits.max_entries = 8;
  limits.max_file_size = 16;
  limits.max_total_size = 64;

  const ZipExtractionResult result =
      UnzipToFolder(zip_data, target_dir, limits);
  EXPECT_FALSE(result.ok());
  EXPECT_NE(result.error().find("per-file"), std::string::npos);
}

TEST(ZipExtractorTest, RejectsTooManyEntries) {
  TempDir temp_dir;
  const fs::path zip_path = temp_dir.path() / "many.zip";
  const fs::path target_dir = temp_dir.path() / "workspace";

  const std::string zip_data = BuildZipArchive(zip_path, {
      {.name = "a.txt", .contents = "1"},
      {.name = "b.txt", .contents = "2"},
      {.name = "c.txt", .contents = "3"},
  });

  ZipExtractionLimits limits;
  limits.max_entries = 2;
  limits.max_file_size = 64;
  limits.max_total_size = 64;

  const ZipExtractionResult result =
      UnzipToFolder(zip_data, target_dir, limits);
  EXPECT_FALSE(result.ok());
  EXPECT_NE(result.error().find("too many entries"), std::string::npos);
}

TEST(ZipExtractorTest, RejectsOversizedArchiveTotal) {
  TempDir temp_dir;
  const fs::path zip_path = temp_dir.path() / "total.zip";
  const fs::path target_dir = temp_dir.path() / "workspace";

  const std::string zip_data = BuildZipArchive(zip_path, {
      {.name = "a.txt", .contents = "1234567890"},
      {.name = "b.txt", .contents = "abcdefghij"},
  });

  ZipExtractionLimits limits;
  limits.max_entries = 8;
  limits.max_file_size = 32;
  limits.max_total_size = 12;

  const ZipExtractionResult result =
      UnzipToFolder(zip_data, target_dir, limits);
  EXPECT_FALSE(result.ok());
  EXPECT_NE(result.error().find("total extraction limit"), std::string::npos);
}

TEST(ZipExtractorTest, RejectsMalformedArchives) {
  TempDir temp_dir;
  const fs::path target_dir = temp_dir.path() / "workspace";

  const ZipExtractionResult result =
      UnzipToFolder("this is not a zip archive", target_dir);
  EXPECT_FALSE(result.ok());
  EXPECT_FALSE(result.error().empty());
}

TEST(ZipExtractorTest, RejectsSymlinkTraversalInsideWorkspace) {
  TempDir temp_dir;
  const fs::path zip_path = temp_dir.path() / "symlink_traversal.zip";
  const fs::path target_dir = temp_dir.path() / "workspace";
  const fs::path outside_dir = temp_dir.path() / "outside_dir";
  const fs::path outside_file = outside_dir / "escape.txt";

  std::error_code error;
  fs::create_directories(target_dir, error);
  fs::create_directories(outside_dir, error);
  ASSERT_FALSE(static_cast<bool>(error));

  if (!CreateDirectorySymlink(outside_dir, target_dir / "link")) {
    GTEST_SKIP() << "Symlink creation is not permitted in this environment";
  }

  const std::string zip_data = BuildZipArchive(zip_path, {
      {.name = "link/escape.txt", .contents = "escape"},
  });

  const ZipExtractionResult result = UnzipToFolder(zip_data, target_dir);
  EXPECT_FALSE(result.ok());
  EXPECT_NE(result.error().find("symlink"), std::string::npos);
  EXPECT_FALSE(fs::exists(outside_file));
}

TEST(ZipExtractorTest, RejectsZipSymlinkEntries) {
  TempDir temp_dir;
  const fs::path zip_path = temp_dir.path() / "zip_symlink.zip";
  const fs::path target_dir = temp_dir.path() / "workspace";

  const std::string zip_data = BuildZipArchive(zip_path, {
      {.name = "link", .contents = "../outside", .set_unix_mode = true,
       .unix_mode = static_cast<mode_t>(S_IFLNK | 0777)},
  });

  const ZipExtractionResult result = UnzipToFolder(zip_data, target_dir);
  EXPECT_FALSE(result.ok());
  EXPECT_NE(result.error().find("symlink"), std::string::npos);
}

}  // namespace
}  // namespace sw_dumper::utils
