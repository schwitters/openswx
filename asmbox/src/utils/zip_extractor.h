// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace sw_dumper::utils {

struct ZipExtractionLimits {
  std::size_t max_entries = 4096;
  std::uint64_t max_file_size = 256ULL * 1024ULL * 1024ULL;
  std::uint64_t max_total_size = 512ULL * 1024ULL * 1024ULL;
};

struct ZipExtractionResult {
  [[nodiscard]] static ZipExtractionResult Ok();
  [[nodiscard]] static ZipExtractionResult Err(std::string message);

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  bool ok_ = false;
  std::string error_;
};

[[nodiscard]] ZipExtractionResult UnzipToFolder(
    const std::string& zip_data, const std::filesystem::path& target_dir,
    const ZipExtractionLimits& limits = {});

}  // namespace sw_dumper::utils
