// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#pragma once

// internal/cmgrhdr_reader.h — Parser for the Contents/CMgrHdr2 binary stream.
//
// This stream uses MFC CArchive serialisation and stores one
// dmConfigMgrHeader_c object that owns a collection of dmConfigHeader_c
// records — one per SolidWorks configuration.
//
// MFC CArchive WriteObject wire format (WORD class tag):
//   0x0000         → null pointer (no object)
//   0x0001..0x7FFF → back-reference to a previously stored object
//   0xFFFF         → new class: schema(u16) + name_len(u16) + name(ASCII)
//                    followed immediately by the object's Serialize data
//   0x8001..0x8FFE → back-reference to a known class (index = tag & 0x7FFF);
//                    followed immediately by the object's Serialize data
//
// WriteCount (variable-length count):
//   WORD < 0xFFFF → value is the count
//   WORD == 0xFFFF → followed by DWORD = actual count
//
// CString (Unicode, as used by SolidWorks):
//   0x00               → empty string
//   0xFF 0xFE <len_b>  → Unicode: len_b chars follow as UTF-16LE
//     where <len_b> is itself variable:
//       0xFF 0xFE WORD  → length = WORD
//       0xFF DWORD      → length = DWORD
//       other byte      → length = that byte
//   0xFF 0xFF 0x00      → empty string
//   0xFF 0xFF <n>       → n chars UTF-16LE
//   <n>                 → n chars ASCII (n > 0, n != 0xFF)
//
// dmConfigHeader_c::Serialize field order (13 fields for modern SW files):
//   1. DWORD  this+0x08  (always)
//   2. CStr   this+0x10  = config name
//   3. DWORD  this+0x30  = configId  (→ Config-N index)
//   4. DWORD  this+0x38  (ver >= kCfgVerUnk1    = 0x053d / 1341)
//   5. CStr   this+0x18  = parent_name  (ver >= kCfgVerParent = 0x07f5 / 2037)
//   6. DWORD  this+0x34  = parent_id  (ver >= kCfgVerParent)
//   7. DWORD  this+0x40  (ver >= kCfgVerUnk2    = 0x0858 / 2136)
//   8. CStr   this+0x20  (ver >= kCfgVerAltName = 0x0a32 / 2610)
//   9. CStr   this+0x28  = alt_name  (ver >= kCfgVerAltName)
//  10. DWORD  this+0x44  (ver >= kCfgVerD10     = 0x0f58 / 3928)
//  11. DWORD  this+0x48  (ver >= kCfgVerD11     = 0x2b86 / 11142)
//  12. DWORD  this+0x50  (ver >= kCfgVerD12     = 0x2f7b / 12155)
//  13. DWORD  this+0x4c  (ver >= kCfgVerD13     = 0x2fb8 / 12216)
//
// The optional fields (4–13) are read greedily: each field is attempted only
// if enough bytes remain.  Because all configs in a given file were written by
// the same SW version, every config has the same set of fields; greedy reading
// advances the cursor correctly between configs.

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>

#include "byte_reader.h"

namespace openswx::internal {

// ── dmConfigHeader_c::Serialize version thresholds ───────────────────────────
//
// These constants name the minimum document version (decimal) at which each
// optional field group was introduced.  Source: disassembly of
// swdocumentmgr.dll, READ path of dmConfigHeader_c::Serialize @ 0x1804e836e.
//
// The version number comes from the _MO_VERSION_NNNNN/ stream name prefix.

/// Fields 1–3 exist in all versions (no gate needed).

/// Field 4: DWORD this+0x38
inline constexpr uint32_t kCfgVerUnk1    = 0x053du;  //  1341 decimal

/// Fields 5–6: CStr parent_name + DWORD parent_id
inline constexpr uint32_t kCfgVerParent  = 0x07f5u;  //  2037 decimal

/// Field 7: DWORD this+0x40
inline constexpr uint32_t kCfgVerUnk2    = 0x0858u;  //  2136 decimal

/// Fields 8–9: CStr this+0x20 + CStr alt_name
inline constexpr uint32_t kCfgVerAltName = 0x0a32u;  //  2610 decimal

/// Field 10: DWORD this+0x44
inline constexpr uint32_t kCfgVerD10     = 0x0f58u;  //  3928 decimal

/// Field 11: DWORD this+0x48
inline constexpr uint32_t kCfgVerD11     = 0x2b86u;  // 11142 decimal

/// Field 12: DWORD this+0x50
inline constexpr uint32_t kCfgVerD12     = 0x2f7bu;  // 12155 decimal

/// Field 13: DWORD this+0x4c
inline constexpr uint32_t kCfgVerD13     = 0x2fb8u;  // 12216 decimal

/// Minimum version for the CMgrHdr2 stream name (vs. the older CMgrHdr).
/// Source: disassembly of swdocumentmgr.dll @ 0x1804eac9c
///   cmp r15d, 0x816 / cmovb rdx, rax
inline constexpr uint32_t kVersionCMgrHdr2 = 0x0816u;  // 2070 decimal

namespace cmgrhdr_detail {

// Read an MFC CString from r.
// On success, advances r and returns the string.
// On any read failure, restores r to its position before the call and returns
// nullopt — so it is always safe to call without an explicit save/restore.
inline std::optional<std::string> ReadCString(ByteReader& r) noexcept {
  const std::size_t start = r.pos();

  auto b0 = r.ReadU8();
  if (!b0) { r.SeekTo(start); return std::nullopt; }

  if (*b0 == 0x00) return std::string{};

  if (*b0 == 0xFF) {
    auto b1 = r.ReadU8();
    if (!b1) { r.SeekTo(start); return std::nullopt; }

    if (*b1 == 0xFE) {
      // Unicode string: FF FE <len> <UTF-16LE>
      auto b2 = r.ReadU8();
      if (!b2) { r.SeekTo(start); return std::nullopt; }

      uint32_t length = 0;
      if (*b2 == 0xFF) {
        auto b3 = r.ReadU8();
        if (!b3) { r.SeekTo(start); return std::nullopt; }
        if (*b3 == 0xFE) {
          auto w = r.ReadU16Le();
          if (!w) { r.SeekTo(start); return std::nullopt; }
          length = *w;
        } else if (*b3 == 0xFF) {
          auto d = r.ReadU32Le();
          if (!d) { r.SeekTo(start); return std::nullopt; }
          length = *d;
        } else {
          length = *b3;
        }
      } else {
        length = *b2;
      }

      auto s = r.ReadUtf16Le(length);
      if (!s) { r.SeekTo(start); return std::nullopt; }
      return s;
    }

    if (*b1 == 0xFF) {
      // FF FF 00 = empty  OR  FF FF <n> <UTF-16LE>
      auto b2 = r.ReadU8();
      if (!b2) { r.SeekTo(start); return std::nullopt; }
      if (*b2 == 0x00) return std::string{};
      auto s = r.ReadUtf16Le(*b2);
      if (!s) { r.SeekTo(start); return std::nullopt; }
      return s;
    }

    // FF <n> <ASCII> — unusual but handled
    const uint8_t n = *b1;
    auto span = r.ReadSpan(n);
    if (span.size() != n) { r.SeekTo(start); return std::nullopt; }
    return std::string(reinterpret_cast<const char*>(span.data()), span.size());
  }

  // ASCII: b0 = length
  const uint8_t n = *b0;
  auto span = r.ReadSpan(n);
  if (span.size() != n) { r.SeekTo(start); return std::nullopt; }
  return std::string(reinterpret_cast<const char*>(span.data()), span.size());
}

// Read an MFC WriteObject class tag.
// Updates class_map with newly encountered class names.
// Returns false only on a hard read error (EOF on the tag WORD itself).
// out_class_name is set to the class name (or an empty string for null/back-ref
// objects, which produce no Serialize data to follow).
inline bool ReadClassTag(ByteReader& r,
                         std::map<int, std::string>& class_map,
                         std::string& out_class_name) noexcept {
  const std::size_t start = r.pos();

  auto tag = r.ReadU16Le();
  if (!tag) return false;

  // Null object — no Serialize data follows.
  if (*tag == 0x0000) { out_class_name.clear(); return true; }

  // Back-reference to a previously stored object — no new Serialize data.
  if (*tag <= 0x7FFF) { out_class_name = "<obj_ref>"; return true; }

  if (*tag == 0xFFFF) {
    // New class: schema(u16) + name_len(u16) + name(ASCII).
    auto schema   = r.ReadU16Le();
    auto name_len = r.ReadU16Le();
    if (!schema || !name_len) { r.SeekTo(start); return false; }

    auto span = r.ReadSpan(*name_len);
    if (span.size() != *name_len) { r.SeekTo(start); return false; }

    out_class_name.assign(reinterpret_cast<const char*>(span.data()),
                          span.size());
    // Class indices on the reading side are pre-seeded with 2 CRuntimeClass
    // entries, so the first user class gets index 3 in a freshly opened
    // CArchive.  Our map starts empty and assigns sequential 1-based indices;
    // the absolute number only matters for back-references.
    const int idx = static_cast<int>(class_map.size()) + 1;
    class_map.emplace(idx, out_class_name);
    return true;
  }

  // 0x8001..0x8FFE: back-reference to a known class.
  const int idx = static_cast<int>(*tag & 0x7FFFu);
  auto it = class_map.find(idx);
  out_class_name = (it != class_map.end()) ? it->second : "<class>";
  return true;
}

}  // namespace cmgrhdr_detail

// Parses a Contents/CMgrHdr2 (or Contents/CMgrHdr) binary stream and returns
// a map of configId → configuration name.
//
// configId maps directly to the N in docProps/Config-N-Properties.xml and the
// related per-configuration stream names.
//
// version is the document version from _MO_VERSION_NNNNN stream names.
// It controls which optional fields are read; pass 0 if unknown (only the
// mandatory three fields will be read, which is sufficient for name resolution).
// Using the correct version is critical — reading too many optional fields
// desynchronises the parser across successive config records.
//
// Returns an empty map on any parse error or if the stream is absent/malformed.
[[nodiscard]] inline std::map<int, std::string> ParseCMgrHdr2(
    std::span<const uint8_t> stream,
    int version = 0) {
  if (stream.empty()) return {};

  ByteReader r(stream);
  std::map<int, std::string> class_map;
  std::map<int, std::string> result;

  using namespace cmgrhdr_detail;

  // ── Outer object: dmConfigMgrHeader_c ──────────────────────────────────────
  {
    std::string outer_class;
    if (!ReadClassTag(r, class_map, outer_class)) return {};
    if (outer_class.empty()) return {};  // null object
  }

  // ── Config count (MFC WriteCount) ─────────────────────────────────────────
  auto w = r.ReadU16Le();
  if (!w) return {};
  uint32_t count = *w;
  if (*w == 0xFFFF) {
    auto d = r.ReadU32Le();
    if (!d) return {};
    count = *d;
  }
  if (count == 0 || count > 10'000) return {};

  // ── Per-config records ────────────────────────────────────────────────────
  for (uint32_t i = 0; i < count; ++i) {
    // Class tag for dmConfigHeader_c
    std::string cfg_class;
    if (!ReadClassTag(r, class_map, cfg_class)) break;

    // Field 1: DWORD this+0x08 (always present)
    if (!r.ReadU32Le()) break;

    // Field 2: CString name (always present)
    auto name = ReadCString(r);
    if (!name) break;

    // Field 3: DWORD configId (always present)
    auto config_id = r.ReadU32Le();
    if (!config_id) break;

    // Record the mapping immediately — the mandatory fields are complete.
    result[static_cast<int>(*config_id)] = std::move(*name);

    // Fields 4–13: version-gated.
    // Each field is read only if the document version meets its threshold.
    // Greedy ("read if bytes remain") is NOT used here because unread fields
    // from the current record would be consumed as data of the next record,
    // desynchronising the parser for all subsequent configs.

    // Field 4: DWORD this+0x38
    if (version < static_cast<int>(kCfgVerUnk1) || r.remaining() < 4) continue;
    r.Skip(4);

    // Fields 5–6: CStr parent_name + DWORD parent_id
    if (version < static_cast<int>(kCfgVerParent)) continue;
    if (!ReadCString(r)) continue;
    if (r.remaining() < 4) continue;
    r.Skip(4);

    // Field 7: DWORD this+0x40
    if (version < static_cast<int>(kCfgVerUnk2) || r.remaining() < 4) continue;
    r.Skip(4);

    // Fields 8–9: CStr this+0x20 + CStr alt_name
    if (version < static_cast<int>(kCfgVerAltName)) continue;
    if (!ReadCString(r)) continue;
    if (!ReadCString(r)) continue;

    // Field 10: DWORD this+0x44
    if (version < static_cast<int>(kCfgVerD10) || r.remaining() < 4) continue;
    r.Skip(4);

    // Field 11: DWORD this+0x48
    if (version < static_cast<int>(kCfgVerD11) || r.remaining() < 4) continue;
    r.Skip(4);

    // Field 12: DWORD this+0x50
    if (version < static_cast<int>(kCfgVerD12) || r.remaining() < 4) continue;
    r.Skip(4);

    // Field 13: DWORD this+0x4c
    if (version < static_cast<int>(kCfgVerD13) || r.remaining() < 4) continue;
    r.Skip(4);
  }

  return result;
}

}  // namespace openswx::internal
