// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#include "utils/zip_extractor.h"

#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zip.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace sw_dumper::utils {

namespace {

using ZipArchivePtr = std::unique_ptr<zip_t, decltype(&zip_close)>;
using ZipFilePtr = std::unique_ptr<zip_file_t, decltype(&zip_fclose)>;
using ZipSourcePtr = std::unique_ptr<zip_source_t, decltype(&zip_source_free)>;

constexpr std::size_t kCopyBufferSize = 64 * 1024;

enum class PathStatusKind {
  kMissing,
  kDirectory,
  kSymlink,
  kOther,
};

struct PathStatus {
  PathStatusKind kind = PathStatusKind::kMissing;
  std::string error_message;
};

std::string ZipErrorToString(const zip_error_t& error) {
  zip_error_t mutable_error = error;
  return zip_error_strerror(&mutable_error);
}

std::string NormalizeEntryName(std::string_view entry_name) {
  std::string normalized(entry_name);
  std::replace(normalized.begin(), normalized.end(), '\\', '/');
  return normalized;
}

PathStatus GetPathStatus(const fs::path& path) {
  struct stat stat_buffer {};
  if (lstat(path.c_str(), &stat_buffer) == 0) {
    if (S_ISLNK(stat_buffer.st_mode)) {
      return {.kind = PathStatusKind::kSymlink};
    }
    if (S_ISDIR(stat_buffer.st_mode)) {
      return {.kind = PathStatusKind::kDirectory};
    }
    return {.kind = PathStatusKind::kOther};
  }

  if (errno == ENOENT) {
    return {.kind = PathStatusKind::kMissing};
  }

  return {
      .kind = PathStatusKind::kOther,
      .error_message = "Cannot inspect extracted path: " + path.string(),
  };
}

bool IsWindowsAbsolutePath(std::string_view entry_name) {
  if (entry_name.size() >= 3 &&
      std::isalpha(static_cast<unsigned char>(entry_name[0])) != 0 &&
      entry_name[1] == ':' &&
      (entry_name[2] == '/' || entry_name[2] == '\\')) {
    return true;
  }

  return entry_name.size() >= 2 &&
         ((entry_name[0] == '\\' && entry_name[1] == '\\') ||
          (entry_name[0] == '/' && entry_name[1] == '/'));
}

bool IsAbsoluteEntryName(std::string_view entry_name) {
  return !entry_name.empty() &&
         (entry_name.front() == '/' || entry_name.front() == '\\' ||
          IsWindowsAbsolutePath(entry_name));
}

bool ContainsParentTraversal(const fs::path& path) {
  for (const fs::path& component : path) {
    if (component == "..") return true;
  }
  return false;
}

bool IsPathWithinRoot(const fs::path& root, const fs::path& candidate) {
  auto root_it = root.begin();
  auto candidate_it = candidate.begin();
  for (; root_it != root.end() && candidate_it != candidate.end();
       ++root_it, ++candidate_it) {
    if (*root_it != *candidate_it) return false;
  }
  return root_it == root.end();
}

ZipExtractionResult EnsureDirectoryInsideRoot(const fs::path& root,
                                              const fs::path& relative_dir,
                                              fs::path* resolved_dir) {
  fs::path current = root;
  for (const fs::path& component : relative_dir) {
    if (component.empty() || component == ".") continue;

    current /= component;

    const PathStatus path_status = GetPathStatus(current);
    if (!path_status.error_message.empty()) {
      return ZipExtractionResult::Err(path_status.error_message);
    }

    if (path_status.kind != PathStatusKind::kMissing) {
      if (path_status.kind == PathStatusKind::kSymlink) {
        return ZipExtractionResult::Err(
            "ZIP entry would traverse through a symlink: " + current.string());
      }
      if (path_status.kind != PathStatusKind::kDirectory) {
        return ZipExtractionResult::Err(
            "ZIP entry collides with a non-directory path: " +
            current.string());
      }
    } else {
      std::error_code create_error;
      if (!fs::create_directory(current, create_error) && create_error) {
        return ZipExtractionResult::Err(
            "Cannot create directory: " + current.string());
      }
    }

    std::error_code canonical_error;
    const fs::path canonical_current = fs::weakly_canonical(current,
                                                            canonical_error);
    if (canonical_error || !IsPathWithinRoot(root, canonical_current)) {
      return ZipExtractionResult::Err(
          "ZIP entry escapes the extraction directory: " + current.string());
    }
    current = canonical_current;
  }

  *resolved_dir = current;
  return ZipExtractionResult::Ok();
}

bool EntryLooksLikeDirectory(std::string_view entry_name) {
  return !entry_name.empty() &&
         (entry_name.back() == '/' || entry_name.back() == '\\');
}

bool IsSymlinkMode(zip_uint8_t opsys, zip_uint32_t attributes) {
  if (opsys != ZIP_OPSYS_UNIX) return false;
  const mode_t mode = static_cast<mode_t>((attributes >> 16) & 0xFFFFu);
  return (mode & S_IFMT) == S_IFLNK;
}

bool IsDirectoryMode(zip_uint8_t opsys, zip_uint32_t attributes) {
  if (opsys != ZIP_OPSYS_UNIX) return false;
  const mode_t mode = static_cast<mode_t>((attributes >> 16) & 0xFFFFu);
  return (mode & S_IFMT) == S_IFDIR;
}

bool IsSupportedFileType(zip_uint8_t opsys, zip_uint32_t attributes,
                         bool directory_entry) {
  if (opsys != ZIP_OPSYS_UNIX) return true;
  const mode_t mode = static_cast<mode_t>((attributes >> 16) & 0xFFFFu);
  const mode_t file_type = mode & S_IFMT;
  if (file_type == 0) return true;
  if (directory_entry) return true;
  return file_type == S_IFREG;
}

ZipExtractionResult ValidateEntryPath(const fs::path& root,
                                      std::string_view original_name,
                                      fs::path* relative_path) {
  if (original_name.empty()) {
    return ZipExtractionResult::Err("ZIP entry has an empty name");
  }
  if (original_name.find('\0') != std::string_view::npos) {
    return ZipExtractionResult::Err("ZIP entry contains a NUL byte");
  }
  if (IsAbsoluteEntryName(original_name)) {
    return ZipExtractionResult::Err("ZIP entry uses an absolute path: " +
                                    std::string(original_name));
  }

  const std::string normalized_name = NormalizeEntryName(original_name);
  fs::path normalized_path(normalized_name);
  if (normalized_path.is_absolute() || ContainsParentTraversal(normalized_path)) {
    return ZipExtractionResult::Err("ZIP entry contains path traversal: " +
                                    normalized_name);
  }

  normalized_path = normalized_path.lexically_normal();
  if (normalized_path.empty() || normalized_path == ".") {
    return ZipExtractionResult::Err("ZIP entry resolves to an empty path");
  }

  const fs::path candidate = (root / normalized_path).lexically_normal();
  if (!IsPathWithinRoot(root, candidate)) {
    return ZipExtractionResult::Err(
        "ZIP entry escapes the extraction directory: " + normalized_name);
  }

  *relative_path = normalized_path;
  return ZipExtractionResult::Ok();
}

ZipExtractionResult ExtractFile(zip_t* archive, zip_uint64_t index,
                                const zip_stat_t& stat,
                                const fs::path& root,
                                const fs::path& relative_path,
                                const ZipExtractionLimits& limits,
                                std::uint64_t* total_extracted_size) {
  fs::path resolved_parent;
  const ZipExtractionResult parent_result =
      EnsureDirectoryInsideRoot(root, relative_path.parent_path(),
                                &resolved_parent);
  if (!parent_result.ok()) return parent_result;

  fs::path output_path = resolved_parent / relative_path.filename();
  const fs::path normalized_output = output_path.lexically_normal();
  if (!IsPathWithinRoot(root, normalized_output)) {
    return ZipExtractionResult::Err(
        "ZIP entry escapes the extraction directory: " + output_path.string());
  }

  const PathStatus output_status = GetPathStatus(output_path);
  if (!output_status.error_message.empty()) {
    return ZipExtractionResult::Err("Cannot inspect output path: " +
                                    output_path.string());
  }
  if (output_status.kind == PathStatusKind::kSymlink) {
    return ZipExtractionResult::Err(
        "ZIP entry would overwrite a symlink: " + output_path.string());
  }
  if (output_status.kind == PathStatusKind::kDirectory) {
    return ZipExtractionResult::Err(
        "ZIP entry collides with a directory path: " + output_path.string());
  }
  if (output_status.kind == PathStatusKind::kOther) {
    // Regular files may be overwritten. Special files were already rejected
    // via archive metadata when that metadata was available.
    std::error_code status_error;
    const fs::file_status existing_status =
        fs::symlink_status(output_path, status_error);
    if (!status_error && fs::is_symlink(existing_status)) {
      return ZipExtractionResult::Err(
          "ZIP entry would overwrite a symlink: " + output_path.string());
    }
  }

  ZipFilePtr input(zip_fopen_index(archive, index, ZIP_FL_UNCHANGED),
                   &zip_fclose);
  if (!input) {
    return ZipExtractionResult::Err("Cannot open ZIP entry: " +
                                    std::string(stat.name));
  }

  std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    return ZipExtractionResult::Err("Cannot create output file: " +
                                    output_path.string());
  }

  std::array<char, kCopyBufferSize> buffer{};
  std::uint64_t file_size = 0;
  while (true) {
    const zip_int64_t bytes_read =
        zip_fread(input.get(), buffer.data(), buffer.size());
    if (bytes_read < 0) {
      return ZipExtractionResult::Err("Cannot read ZIP entry: " +
                                      std::string(stat.name));
    }
    if (bytes_read == 0) break;

    file_size += static_cast<std::uint64_t>(bytes_read);
    if (file_size > limits.max_file_size) {
      return ZipExtractionResult::Err(
          "ZIP entry exceeds the per-file extraction limit: " +
          std::string(stat.name));
    }

    *total_extracted_size += static_cast<std::uint64_t>(bytes_read);
    if (*total_extracted_size > limits.max_total_size) {
      return ZipExtractionResult::Err(
          "ZIP archive exceeds the total extraction limit");
    }

    output.write(buffer.data(), static_cast<std::streamsize>(bytes_read));
    if (!output) {
      return ZipExtractionResult::Err("Cannot write output file: " +
                                      output_path.string());
    }
  }

  if ((stat.valid & ZIP_STAT_SIZE) != 0 && stat.size != file_size) {
    return ZipExtractionResult::Err(
        "ZIP entry size mismatch detected while extracting: " +
        std::string(stat.name));
  }

  return ZipExtractionResult::Ok();
}

}  // namespace

ZipExtractionResult ZipExtractionResult::Ok() {
  ZipExtractionResult result;
  result.ok_ = true;
  return result;
}

ZipExtractionResult ZipExtractionResult::Err(std::string message) {
  ZipExtractionResult result;
  result.ok_ = false;
  result.error_ = std::move(message);
  return result;
}

ZipExtractionResult UnzipToFolder(const std::string& zip_data,
                                  const fs::path& target_dir,
                                  const ZipExtractionLimits& limits) {
  if (limits.max_entries == 0 || limits.max_file_size == 0 ||
      limits.max_total_size == 0) {
    return ZipExtractionResult::Err("ZIP extraction limits must be non-zero");
  }

  std::error_code create_error;
  if (!fs::create_directories(target_dir, create_error) && create_error) {
    return ZipExtractionResult::Err("Cannot create extraction directory: " +
                                    target_dir.string());
  }

  std::error_code canonical_error;
  const fs::path canonical_root =
      fs::weakly_canonical(target_dir, canonical_error);
  if (canonical_error) {
    return ZipExtractionResult::Err(
        "Cannot resolve extraction directory: " + target_dir.string());
  }

  zip_error_t zip_error;
  zip_error_init(&zip_error);
  ZipSourcePtr source(
      zip_source_buffer_create(zip_data.data(), zip_data.size(), 0, &zip_error),
      &zip_source_free);
  if (!source) {
    const std::string error_message = ZipErrorToString(zip_error);
    zip_error_fini(&zip_error);
    return ZipExtractionResult::Err("Cannot open ZIP source: " +
                                    error_message);
  }

  ZipArchivePtr archive(zip_open_from_source(source.get(), ZIP_RDONLY,
                                             &zip_error),
                        &zip_close);
  if (!archive) {
    const std::string error_message = ZipErrorToString(zip_error);
    zip_error_fini(&zip_error);
    return ZipExtractionResult::Err("Cannot open ZIP archive: " +
                                    error_message);
  }
  source.release();
  zip_error_fini(&zip_error);

  const zip_int64_t raw_num_entries = zip_get_num_entries(archive.get(), 0);
  if (raw_num_entries < 0) {
    return ZipExtractionResult::Err("Cannot enumerate ZIP archive entries");
  }

  const std::uint64_t num_entries =
      static_cast<std::uint64_t>(raw_num_entries);
  if (num_entries > limits.max_entries) {
    return ZipExtractionResult::Err("ZIP archive contains too many entries");
  }

  std::uint64_t declared_total_size = 0;
  std::uint64_t extracted_total_size = 0;
  for (zip_uint64_t index = 0; index < num_entries; ++index) {
    zip_stat_t stat;
    zip_stat_init(&stat);
    if (zip_stat_index(archive.get(), index, ZIP_FL_UNCHANGED, &stat) != 0) {
      return ZipExtractionResult::Err("Cannot read ZIP entry metadata");
    }
    if ((stat.valid & ZIP_STAT_NAME) == 0 || stat.name == nullptr) {
      return ZipExtractionResult::Err("ZIP entry is missing a valid name");
    }

    const std::string_view entry_name(stat.name);
    fs::path relative_path;
    const ZipExtractionResult path_result =
        ValidateEntryPath(canonical_root, entry_name, &relative_path);
    if (!path_result.ok()) return path_result;

    zip_uint8_t opsys = ZIP_OPSYS_DEFAULT;
    zip_uint32_t attributes = 0;
    const bool has_external_attributes =
        zip_file_get_external_attributes(archive.get(), index,
                                         ZIP_FL_UNCHANGED, &opsys,
                                         &attributes) == 0;
    const bool directory_entry =
        EntryLooksLikeDirectory(entry_name) ||
        (has_external_attributes && IsDirectoryMode(opsys, attributes));

    if (has_external_attributes) {
      if (IsSymlinkMode(opsys, attributes)) {
        return ZipExtractionResult::Err(
            "ZIP symlink entries are not supported: " +
            std::string(entry_name));
      }
      if (!IsSupportedFileType(opsys, attributes, directory_entry)) {
        return ZipExtractionResult::Err(
            "ZIP entry uses an unsupported file type: " +
            std::string(entry_name));
      }
    }

    if (!directory_entry && (stat.valid & ZIP_STAT_SIZE) != 0) {
      declared_total_size += stat.size;
      if (stat.size > limits.max_file_size) {
        return ZipExtractionResult::Err(
            "ZIP entry exceeds the per-file extraction limit: " +
            std::string(entry_name));
      }
      if (declared_total_size > limits.max_total_size) {
        return ZipExtractionResult::Err(
            "ZIP archive exceeds the total extraction limit");
      }
    }

    if (directory_entry) {
      fs::path resolved_dir;
      const ZipExtractionResult dir_result =
          EnsureDirectoryInsideRoot(canonical_root, relative_path,
                                    &resolved_dir);
      if (!dir_result.ok()) return dir_result;
      continue;
    }

    const ZipExtractionResult file_result =
        ExtractFile(archive.get(), index, stat, canonical_root, relative_path,
                    limits, &extracted_total_size);
    if (!file_result.ok()) return file_result;
  }

  return ZipExtractionResult::Ok();
}

}  // namespace sw_dumper::utils
