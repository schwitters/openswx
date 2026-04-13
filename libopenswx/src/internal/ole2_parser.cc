// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#include "internal/ole2_parser.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "internal/byte_reader.h"
#include "internal/decompressor.h"

namespace openswx::internal {

namespace {

// ── OLE2 constants ──────────────────────────────────────────────────────────

constexpr uint8_t kOle2Magic[] = {0xD0, 0xCF, 0x11, 0xE0,
                                   0xA1, 0xB1, 0x1A, 0xE1};

// Special sector / directory-entry sentinel values.
constexpr uint32_t kFreeSect   = 0xFFFFFFFF;
constexpr uint32_t kEndOfChain = 0xFFFFFFFE;
constexpr uint32_t kFatSect    = 0xFFFFFFFD;
constexpr uint32_t kDifSect    = 0xFFFFFFFC;
constexpr uint32_t kNoEntry    = 0xFFFFFFFF;

// Directory entry object types.
constexpr uint8_t kTypeEmpty   = 0;
constexpr uint8_t kTypeStorage = 1;
constexpr uint8_t kTypeStream  = 2;
constexpr uint8_t kTypeRoot    = 5;

// Size of a directory entry (always 128 bytes).
constexpr std::size_t kDirEntrySize = 128;

// ── Data structures ──────────────────────────────────────────────────────────

struct OleHeader {
  uint32_t sector_size{512};
  uint32_t mini_sector_size{64};
  uint32_t mini_stream_cutoff{4096};
  uint32_t first_dir_sector{kEndOfChain};
  uint32_t first_mini_fat_sector{kEndOfChain};
  uint32_t first_difat_sector{kEndOfChain};
  std::array<uint32_t, 109> difat{};
};

struct DirEntry {
  std::string name;
  uint8_t type{kTypeEmpty};
  uint32_t left{kNoEntry};
  uint32_t right{kNoEntry};
  uint32_t child{kNoEntry};
  uint32_t start_sector{kEndOfChain};
  uint64_t size{0};
};

// ── OleReader ────────────────────────────────────────────────────────────────

class OleReader {
 public:
  explicit OleReader(std::span<const uint8_t> data) noexcept : data_(data) {}

  [[nodiscard]] bool Init();
  void PopulateStreams(StreamMap& out) const;

 private:
  [[nodiscard]] bool ParseHeader();
  [[nodiscard]] bool BuildFat();
  [[nodiscard]] bool BuildMiniFat();
  [[nodiscard]] bool ReadDirectory();
  [[nodiscard]] bool ReadMiniStream();

  // Read a chain of regular sectors starting at start_sector, up to size bytes.
  [[nodiscard]] std::vector<uint8_t> ReadSectorChain(uint32_t start_sector,
                                                      uint64_t size) const;

  // Read a chain of mini-sectors, up to size bytes.
  [[nodiscard]] std::vector<uint8_t> ReadMiniSectorChain(
      uint32_t start_mini_sector, uint64_t size) const;

  // Decode and return the data for a directory entry.
  [[nodiscard]] std::vector<uint8_t> ReadEntryData(
      const DirEntry& entry) const;

  // Recursively enumerate the red-black sibling tree rooted at node_idx.
  // For each stream entry, appends to out with the full path.
  void TraverseSiblingTree(uint32_t node_idx, const std::string& parent_path,
                            StreamMap& out) const;

  // Returns the absolute file offset of sector N.
  [[nodiscard]] std::size_t SectorOffset(uint32_t sector) const noexcept {
    return 512 + static_cast<std::size_t>(sector) * header_.sector_size;
  }

  std::span<const uint8_t> data_;
  OleHeader header_;
  std::vector<uint32_t> fat_;
  std::vector<uint32_t> mini_fat_;
  std::vector<DirEntry> dir_;
  std::vector<uint8_t> mini_stream_data_;
};

// ── OleReader::Init and helpers ──────────────────────────────────────────────

bool OleReader::ParseHeader() {
  if (data_.size() < 512) return false;

  // Sector size: 2^(sector_size_shift).
  // Standard: 2^9=512 (v3) or 2^12=4096 (v4).
  // Very old SW files sometimes use 2^6=64; allow range 6..14.
  // MS-CFB spec: ss_shift at offset 30, mss_shift at offset 32.
  uint16_t ss_shift = static_cast<uint16_t>(data_[30]) |
                      (static_cast<uint16_t>(data_[31]) << 8);
  if (ss_shift < 6 || ss_shift > 14) return false;
  header_.sector_size = 1u << ss_shift;

  uint16_t mss_shift = static_cast<uint16_t>(data_[32]) |
                       (static_cast<uint16_t>(data_[33]) << 8);
  header_.mini_sector_size = (mss_shift < 14) ? (1u << mss_shift) : 64;

  header_.mini_stream_cutoff = ReadU32LeAt(data_, 56);
  header_.first_dir_sector   = ReadU32LeAt(data_, 48);
  header_.first_mini_fat_sector = ReadU32LeAt(data_, 60);
  header_.first_difat_sector = ReadU32LeAt(data_, 68);

  for (int i = 0; i < 109; ++i)
    header_.difat[i] = ReadU32LeAt(data_, 76 + i * 4);

  return true;
}

bool OleReader::BuildFat() {
  // Collect FAT sector indices from header DIFAT and any chained DIFAT sectors.
  std::vector<uint32_t> fat_sectors;
  fat_sectors.reserve(128);

  for (uint32_t s : header_.difat) {
    if (s == kFreeSect || s == kEndOfChain || s == kFatSect) break;
    fat_sectors.push_back(s);
  }

  uint32_t difat_sec = header_.first_difat_sector;
  while (difat_sec != kEndOfChain && difat_sec != kFreeSect) {
    std::size_t off = SectorOffset(difat_sec);
    if (off >= data_.size()) break;
    uint32_t entries = header_.sector_size / 4;
    for (uint32_t i = 0; i + 1 < entries; ++i) {
      uint32_t s = ReadU32LeAt(data_, off + i * 4);
      if (s == kFreeSect || s == kEndOfChain || s == kFatSect) break;
      fat_sectors.push_back(s);
    }
    // Last entry in the DIFAT sector is the next DIFAT sector.
    difat_sec = ReadU32LeAt(data_, off + (entries - 1) * 4);
  }

  // Read FAT entries from each FAT sector.
  uint32_t entries_per_sector = header_.sector_size / 4;
  fat_.clear();
  fat_.reserve(fat_sectors.size() * entries_per_sector);

  for (uint32_t fs : fat_sectors) {
    std::size_t off = SectorOffset(fs);
    for (uint32_t i = 0; i < entries_per_sector; ++i) {
      fat_.push_back(ReadU32LeAt(data_, off + i * 4));
    }
  }

  return !fat_.empty();
}

bool OleReader::BuildMiniFat() {
  uint32_t sec = header_.first_mini_fat_sector;
  uint32_t entries_per_sector = header_.sector_size / 4;

  while (sec != kEndOfChain && sec != kFreeSect) {
    std::size_t off = SectorOffset(sec);
    if (off >= data_.size()) break;
    for (uint32_t i = 0; i < entries_per_sector; ++i) {
      mini_fat_.push_back(ReadU32LeAt(data_, off + i * 4));
    }
    if (sec >= fat_.size()) break;
    sec = fat_[sec];
  }
  return true;
}

bool OleReader::ReadDirectory() {
  // Read all directory sectors into a flat byte buffer.
  std::vector<uint8_t> dir_data =
      ReadSectorChain(header_.first_dir_sector,
                      UINT64_MAX);  // read until chain ends

  std::size_t entry_count = dir_data.size() / kDirEntrySize;
  dir_.resize(entry_count);

  for (std::size_t i = 0; i < entry_count; ++i) {
    const uint8_t* e = dir_data.data() + i * kDirEntrySize;
    DirEntry& de = dir_[i];

    // Name: UTF-16LE, up to 64 bytes (32 code units).
    uint16_t name_bytes = static_cast<uint16_t>(e[64]) |
                          (static_cast<uint16_t>(e[65]) << 8);
    if (name_bytes > 64) name_bytes = 64;
    uint16_t name_units = (name_bytes >= 2) ? (name_bytes / 2 - 1) : 0;

    for (uint16_t j = 0; j < name_units; ++j) {
      uint16_t cp = static_cast<uint16_t>(e[j * 2]) |
                    (static_cast<uint16_t>(e[j * 2 + 1]) << 8);
      if (cp == 0) break;
      // Convert BMP code points to UTF-8.
      if (cp < 0x80) {
        de.name += static_cast<char>(cp);
      } else if (cp < 0x800) {
        de.name += static_cast<char>(0xC0 | (cp >> 6));
        de.name += static_cast<char>(0x80 | (cp & 0x3F));
      } else {
        de.name += static_cast<char>(0xE0 | (cp >> 12));
        de.name += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        de.name += static_cast<char>(0x80 | (cp & 0x3F));
      }
    }

    de.type  = e[66];
    de.left  = ReadU32LeAt({e, kDirEntrySize}, 68);
    de.right = ReadU32LeAt({e, kDirEntrySize}, 72);
    de.child = ReadU32LeAt({e, kDirEntrySize}, 76);
    de.start_sector = ReadU32LeAt({e, kDirEntrySize}, 116);
    de.size = static_cast<uint64_t>(ReadU32LeAt({e, kDirEntrySize}, 120));
  }

  return !dir_.empty();
}

bool OleReader::ReadMiniStream() {
  // The root entry (index 0) contains the mini-stream container.
  // Very old SW OLE2 files sometimes store the root entry with type=storage(1)
  // instead of the spec-mandated type=root(5).  Accept both.
  if (dir_.empty()) return false;
  if (dir_[0].type != kTypeRoot && dir_[0].type != kTypeStorage) return false;
  mini_stream_data_ =
      ReadSectorChain(dir_[0].start_sector, dir_[0].size);
  return true;
}

bool OleReader::Init() {
  return ParseHeader() && BuildFat() && BuildMiniFat() &&
         ReadDirectory() && ReadMiniStream();
}

// ── Sector chain readers ─────────────────────────────────────────────────────

std::vector<uint8_t> OleReader::ReadSectorChain(uint32_t start_sector,
                                                  uint64_t max_size) const {
  std::vector<uint8_t> result;

  uint32_t sec = start_sector;
  while (sec != kEndOfChain && sec != kFreeSect && sec != kFatSect) {
    std::size_t off = SectorOffset(sec);
    if (off >= data_.size()) break;

    std::size_t to_read = std::min(
        static_cast<std::size_t>(header_.sector_size),
        data_.size() - off);

    if (max_size != UINT64_MAX) {
      to_read = std::min(to_read,
                         static_cast<std::size_t>(max_size - result.size()));
    }

    result.insert(result.end(), data_.begin() + off,
                  data_.begin() + off + to_read);

    if (max_size != UINT64_MAX && result.size() >= max_size) break;

    if (sec >= fat_.size()) break;
    sec = fat_[sec];
  }

  return result;
}

std::vector<uint8_t> OleReader::ReadMiniSectorChain(
    uint32_t start_mini_sector, uint64_t size) const {
  std::vector<uint8_t> result;

  uint32_t ms = start_mini_sector;
  while (ms != kEndOfChain && ms != kFreeSect && result.size() < size) {
    std::size_t off =
        static_cast<std::size_t>(ms) * header_.mini_sector_size;
    if (off >= mini_stream_data_.size()) break;

    std::size_t to_read = std::min(
        static_cast<std::size_t>(header_.mini_sector_size),
        static_cast<std::size_t>(size - result.size()));
    to_read = std::min(to_read, mini_stream_data_.size() - off);

    result.insert(result.end(), mini_stream_data_.begin() + off,
                  mini_stream_data_.begin() + off + to_read);

    if (ms >= mini_fat_.size()) break;
    ms = mini_fat_[ms];
  }

  return result;
}

std::vector<uint8_t> OleReader::ReadEntryData(const DirEntry& entry) const {
  if (entry.type != kTypeStream) return {};
  if (entry.size < header_.mini_stream_cutoff) {
    return ReadMiniSectorChain(entry.start_sector, entry.size);
  }
  return ReadSectorChain(entry.start_sector, entry.size);
}

// ── Directory traversal ──────────────────────────────────────────────────────

void OleReader::TraverseSiblingTree(uint32_t node_idx,
                                    const std::string& parent_path,
                                    StreamMap& out) const {
  if (node_idx == kNoEntry || node_idx >= dir_.size()) return;

  const DirEntry& node = dir_[node_idx];
  if (node.type == kTypeEmpty) return;

  // In-order traversal of the red-black sibling tree.
  TraverseSiblingTree(node.left, parent_path, out);

  // Build the full path for this entry.
  std::string path =
      parent_path.empty() ? node.name : parent_path + "/" + node.name;

  // Skip entries whose path contains control characters (OLE2 metadata).
  bool has_control = false;
  for (char c : path)
    if (static_cast<unsigned char>(c) < 0x20) { has_control = true; break; }

  if (!has_control) {
    if (node.type == kTypeStream) {
      if (out.find(path) == out.end()) {
        std::vector<uint8_t> raw = ReadEntryData(node);

        // Decompress __Zip-suffixed streams (raw deflate).
        static constexpr std::string_view kZipSuffix = "__Zip";
        if (path.size() > kZipSuffix.size() &&
            path.ends_with(kZipSuffix)) {
          auto decompressed = InflateRaw(raw);
          if (decompressed) {
            std::string base_name =
                path.substr(0, path.size() - kZipSuffix.size());
            if (out.find(base_name) == out.end())
              out.emplace(std::move(base_name), std::move(*decompressed));
          }
        } else {
          out.emplace(path, std::move(raw));
        }
      }
    } else if (node.type == kTypeStorage && node.child != kNoEntry) {
      // Recurse into the storage's children tree.
      TraverseSiblingTree(node.child, path, out);
    }
  }

  TraverseSiblingTree(node.right, parent_path, out);
}

void OleReader::PopulateStreams(StreamMap& out) const {
  if (dir_.empty()) return;
  // Root entry (index 0) is the virtual root storage.
  // Its children tree starts at dir_[0].child.
  if (dir_[0].child != kNoEntry)
    TraverseSiblingTree(dir_[0].child, "", out);
}

}  // namespace

// ── Public API ───────────────────────────────────────────────────────────────

bool IsOle2(std::span<const uint8_t> data) noexcept {
  if (data.size() < 8) return false;
  for (int i = 0; i < 8; ++i)
    if (data[i] != kOle2Magic[i]) return false;
  return true;
}

bool ParseOle2Format(std::span<const uint8_t> data, StreamMap& streams) {
  OleReader reader(data);
  if (!reader.Init()) return false;
  reader.PopulateStreams(streams);
  return true;
}

}  // namespace openswx::internal
