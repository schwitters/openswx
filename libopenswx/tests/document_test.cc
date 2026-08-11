// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "openswx/document.h"
#include "test_helpers.h"

namespace openswx {
namespace {

class TempFile {
 public:
  TempFile(std::string filename, std::span<const uint8_t> contents)
      : path_(std::filesystem::temp_directory_path() / std::move(filename)) {
    std::ofstream stream(path_, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(contents.data()),
                 static_cast<std::streamsize>(contents.size()));
    stream.close();
  }

  ~TempFile() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

TEST(SwxDocumentTest, RejectsZipOpcInputGracefully) {
  const std::array<uint8_t, 4> zip_magic = {0x50, 0x4B, 0x03, 0x04};
  TempFile file("openswx-zipopc-test.SLDPRT", zip_magic);

  const auto result = SwxDocument::Open(file.path());
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().find("ZIP/OPC"), std::string::npos);
}

TEST(SwxDocumentTest, RejectsPre2015Ole2InputGracefully) {
  const std::array<uint8_t, 8> ole_magic = {
      0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
  TempFile file("openswx-ole2-test.SLDASM", ole_magic);

  const auto result = SwxDocument::Open(file.path());
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().find("pre-2015"), std::string::npos);
}

TEST(SwxDocumentTest, RejectsModernFileWithoutRecoverableStreams) {
  const std::array<uint8_t, 1> payload = {0x42};
  std::vector<uint8_t> file_data =
      test::MakeModernChunkFile("PreviewPNG", payload);
  const std::size_t name_size_offset = 8 + 0x1A;
  file_data[name_size_offset] = 0xFF;
  file_data[name_size_offset + 1] = 0x02;
  file_data[name_size_offset + 2] = 0x00;
  file_data[name_size_offset + 3] = 0x00;
  TempFile file("openswx-empty-streams-test.SLDDRW", file_data);

  const auto result = SwxDocument::Open(file.path());
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().find("No streams found"), std::string::npos);
}

}  // namespace
}  // namespace openswx
