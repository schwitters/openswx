// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#include "openswx/document.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <fstream>
#include <set>

#include "internal/cmgrhdr_reader.h"
#include "internal/decompressor.h"
#include "internal/modern_parser.h"
#include "internal/ole2_parser.h"
#include "internal/sheet_name_reader.h"
#include "internal/stream_store.h"
#include "internal/xml_readers.h"

namespace openswx {

namespace {

using internal::StreamMap;

// ── File I/O ─────────────────────────────────────────────────────────────────

Result<std::vector<uint8_t>> ReadFile(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f.is_open())
    return Result<std::vector<uint8_t>>::Err("Cannot open file: " +
                                              path.string());

  auto size = f.tellg();
  if (size <= 0)
    return Result<std::vector<uint8_t>>::Err("File is empty: " +
                                              path.string());

  std::vector<uint8_t> buf(static_cast<std::size_t>(size));
  f.seekg(0);
  f.read(reinterpret_cast<char*>(buf.data()), size);
  if (!f)
    return Result<std::vector<uint8_t>>::Err("Read error: " + path.string());

  return Result<std::vector<uint8_t>>::Ok(std::move(buf));
}

// ── Format detection and parsing ─────────────────────────────────────────────
//
// Detection order:
//   1. OLE2 (pre-2015) — magic D0 CF 11 E0  → error (unsupported)
//   2. ZIP / OPC (3DExperience) — magic 50 4B 03 04  → error (unsupported)
//   3. SW chunk format (2015+) — marker 14 00 06 00 08 00 in first 64 B
//
// The modern format has a file-specific 4-byte magic that varies per file,
// so content-based detection uses the stable inner chunk marker instead.

Result<StreamMap> ParseFile(std::span<const uint8_t> data) {
  StreamMap streams;

  if (internal::IsOle2(data)) {
    return Result<StreamMap>::Err(
        "SolidWorks pre-2015 format (OLE2/CFB) is not supported. "
        "Resave the file with SolidWorks 2015 or later.");

  } else if (internal::IsZipOpc(data)) {
    // 3DExperience / Open Packaging Convention — not yet supported.
    // The container layer needs a dedicated ZIP/OPC stream provider.
    return Result<StreamMap>::Err(
        "ZIP/OPC (3DExperience) format is not yet supported");

  } else if (internal::IsModernSwx(data)) {
    if (!internal::ParseModernFormat(data, streams))
      return Result<StreamMap>::Err("Failed to parse modern SW format");

  } else {
    return Result<StreamMap>::Err(
        "Unrecognised file format "
        "(not OLE2, ZIP/OPC, or modern SolidWorks chunk format)");
  }

  return Result<StreamMap>::Ok(std::move(streams));
}

// ── Document type ─────────────────────────────────────────────────────────────

DocumentType InferDocumentType(const std::filesystem::path& path) {
  std::string ext = path.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::toupper);
  if (ext == ".SLDASM") return DocumentType::kAssembly;
  if (ext == ".SLDDRW") return DocumentType::kDrawing;
  return DocumentType::kPart;
}

// ── Version extraction ────────────────────────────────────────────────────────

// SolidWorks encodes the document version as a decimal number in a stream
// *name* prefix: "_MO_VERSION_NNNNN/SubStream".  There is no separate content
// stream (verified by inspection of known files).  The DLL writes to a stream
// named "_MO_VERSION_" on save (confirmed by disassembly), but this content
// stream is not present in the modern chunk format used for reading.
//
// Notable version thresholds (decimal):
//   2070 (0x0816) — CMgrHdr → CMgrHdr2 switch
//   See cmgrhdr_reader.h for the full list of per-field version gates.
//
// Returns the largest version number found; 0 if none.
int ExtractVersion(const StreamMap& streams) {
  int version = 0;
  constexpr std::string_view kPrefix = "_MO_VERSION_";

  for (const auto& [name, _] : streams) {
    if (!name.starts_with(kPrefix)) continue;
    std::string_view suffix = std::string_view(name).substr(kPrefix.size());
    auto slash = suffix.find('/');
    if (slash == std::string_view::npos) continue;
    std::string_view digits = suffix.substr(0, slash);

    int v = 0;
    auto [ptr, ec] =
        std::from_chars(digits.data(), digits.data() + digits.size(), v);
    if (ec == std::errc{} && v > version) version = v;
  }
  return version;
}

// ── PNG extraction ────────────────────────────────────────────────────────────

// Some preview streams have bytes before the PNG magic header.
// This function locates and returns just the PNG data.
std::vector<uint8_t> ExtractPng(std::span<const uint8_t> stream_data) {
  constexpr uint8_t kPngMagic[] = {0x89, 0x50, 0x4E, 0x47,
                                    0x0D, 0x0A, 0x1A, 0x0A};
  auto it = std::search(stream_data.begin(), stream_data.end(),
                         std::begin(kPngMagic), std::end(kPngMagic));
  if (it == stream_data.end()) return {};
  return std::vector<uint8_t>(it, stream_data.end());
}

std::vector<uint8_t> GetPreviewPng(const StreamMap& streams,
                                    const std::string& stream_name) {
  auto span = internal::GetStream(streams, stream_name);
  if (span.empty()) return {};
  return ExtractPng(span);
}

// ── Configuration discovery ───────────────────────────────────────────────────

// Returns the sorted set of configuration indices N found in the stream map
// by looking for "Contents/Config-N" or "docProps/Config-N-Properties.xml".
std::set<int> DiscoverConfigIndices(const StreamMap& streams) {
  std::set<int> indices;

  for (const auto& [name, _] : streams) {
    // Match "docProps/Config-N-..." or "Contents/Config-N..." patterns.
    for (const char* prefix :
         {"docProps/Config-", "Contents/Config-"}) {
      if (!name.starts_with(prefix)) continue;
      std::string_view rest =
          std::string_view(name).substr(std::strlen(prefix));
      auto dash = rest.find('-');
      std::string_view digits =
          (dash != std::string_view::npos) ? rest.substr(0, dash) : rest;

      // Also accept just "Contents/Config-N" (no trailing dash).
      if (digits.empty()) {
        // rest is "N" without a dash — check digits only.
        digits = rest;
        auto non_digit = digits.find_first_not_of("0123456789");
        if (non_digit != std::string_view::npos) digits = digits.substr(0, non_digit);
      }

      int n = 0;
      auto [ptr, ec] =
          std::from_chars(digits.data(), digits.data() + digits.size(), n);
      if (ec == std::errc{}) indices.insert(n);
      break;
    }
  }
  return indices;
}

// ── Config name resolution ────────────────────────────────────────────────────

// Tries to determine the human-readable name of configuration index N.
// Sources (in priority order):
//   0. CMgrHdr2 binary stream (most authoritative — contains all config names)
//   1. COMPINSTANCETREE swModel/@swConfigurationName
//   2. Cutlist XML <Configuration Name="...">
//   3. ISolidWorksInformation "SW-Configuration Name" (active config only)
//   4. Empty string (caller may use index as fallback)
std::string ResolveConfigName(
    const StreamMap& streams,
    int n,
    const std::map<int, std::string>& compinstancetree_names,
    const std::map<int, std::string>& cmgrhdr2_names) {
  // 0. CMgrHdr2
  {
    auto it = cmgrhdr2_names.find(n);
    if (it != cmgrhdr2_names.end() && !it->second.empty())
      return it->second;
  }

  // 1. COMPINSTANCETREE
  auto it = compinstancetree_names.find(n);
  if (it != compinstancetree_names.end() && !it->second.empty())
    return it->second;

  // 2. Cutlist XML <Configuration Name="...">
  std::string cl_key = "docProps/Config-" + std::to_string(n) +
                       "-Cutlist-Properties.xml";
  auto cl_span = internal::GetStream(streams, cl_key);
  if (!cl_span.empty()) {
    // Quick attribute parse: <Configuration id="N" Name="...">
    // Use pugixml via xml_readers — but we only need one attribute.
    // Load it minimally to avoid a full cut-list parse just for the name.
    // We delegate to ParseCutlistXml's sibling: just look for the Name attr.
    // For simplicity, use a tiny inline parse here.
    std::string_view sv(reinterpret_cast<const char*>(cl_span.data()),
                        cl_span.size());
    auto name_pos = sv.find("Name=\"");
    if (name_pos != std::string_view::npos) {
      auto start = name_pos + 6;
      auto end   = sv.find('"', start);
      if (end != std::string_view::npos && end > start) {
        std::string name(sv.substr(start, end - start));
        // Unescape XML entities (&lt; &gt; &amp;).
        for (auto& [from, to] :
             std::initializer_list<std::pair<std::string_view, std::string_view>>{
                 {"&lt;", "<"}, {"&gt;", ">"}, {"&amp;", "&"}}) {
          std::string result;
          std::size_t pos = 0;
          while (true) {
            auto f = name.find(from, pos);
            if (f == std::string::npos) { result += name.substr(pos); break; }
            result += name.substr(pos, f - pos);
            result += to;
            pos = f + from.size();
          }
          name = std::move(result);
        }
        if (!name.empty()) return name;
      }
    }
  }

  return {};
}

// ── Configuration builder ─────────────────────────────────────────────────────

Configuration BuildConfiguration(
    const StreamMap& streams,
    int n,
    const std::map<int, std::string>& compinstancetree_names,
    const std::map<int, std::string>& cmgrhdr2_names,
    DocumentType doc_type) {
  Configuration cfg;
  cfg.index = n;
  cfg.name  = ResolveConfigName(streams, n, compinstancetree_names,
                                 cmgrhdr2_names);

  // Custom properties: merge file-level (custom.xml) with config-level.
  auto global_span =
      internal::GetStream(streams, "docProps/custom.xml");
  if (!global_span.empty())
    cfg.properties = internal::ParsePropertyXml(global_span);

  std::string cfg_props_key =
      "docProps/Config-" + std::to_string(n) + "-Properties.xml";
  auto cfg_props_span = internal::GetStream(streams, cfg_props_key);
  if (!cfg_props_span.empty()) {
    // Config-level properties override global ones with the same name.
    auto cfg_props = internal::ParsePropertyXml(cfg_props_span);
    for (auto& [k, v] : cfg_props)
      cfg.properties.insert_or_assign(k, std::move(v));
  }

  // Mass properties from ISolidWorksInformation.xml.
  auto sw_info_span =
      internal::GetStream(streams, "docProps/ISolidWorksInformation.xml");
  if (!sw_info_span.empty())
    cfg.mass_properties = internal::ParseMassProperties(sw_info_span, n);

  // Preview PNG.
  cfg.preview_png = GetPreviewPng(
      streams, "Config-" + std::to_string(n) + "-PreviewPNG");
  if (cfg.preview_png.empty())
    cfg.preview_png = GetPreviewPng(streams, "PreviewPNG");

  // Cut list (weldment parts).
  std::string cl_key =
      "docProps/Config-" + std::to_string(n) + "-Cutlist-Properties.xml";
  auto cl_span = internal::GetStream(streams, cl_key);
  if (!cl_span.empty())
    cfg.cut_list = internal::ParseCutlistXml(cl_span);

  // Assembly components.
  if (doc_type == DocumentType::kAssembly) {
    auto cit_span =
        internal::GetStream(streams, "swXmlContents/COMPINSTANCETREE");
    if (!cit_span.empty())
      cfg.components = internal::ParseComponents(cit_span, n);
  }

  // Fallback 1: if name still empty, check the "Configuration" user property
  // stored inside Config-N-Properties.xml.  SolidWorks often writes the
  // config name there as a self-referential property.
  if (cfg.name.empty()) {
    auto it = cfg.properties.find("Configuration");
    if (it != cfg.properties.end() && !it->second.empty())
      cfg.name = it->second;
  }

  // Fallback 2: for config index 0, if still unnamed, use the active config
  // name from ISolidWorksInformation.xml.  This is only reliable here when
  // the Configuration property above was absent (single-config files where
  // no per-config XML properties provide the name).
  if (cfg.name.empty() && n == 0) {
    auto sw_info_span2 =
        internal::GetStream(streams, "docProps/ISolidWorksInformation.xml");
    if (!sw_info_span2.empty()) {
      auto info = internal::ParseSwInformation(sw_info_span2);
      auto cfg_it = info.find("SW-Configuration Name");
      if (cfg_it != info.end() && !cfg_it->second.empty())
        cfg.name = cfg_it->second;
    }
  }

  return cfg;
}

// ── Drawing sheet builder ─────────────────────────────────────────────────────

std::vector<Sheet> BuildSheets(const StreamMap& streams) {
  // Primary source: swXmlContents/KeyWords — contains sheet names AND views.
  auto kw_span = internal::GetStream(streams, "swXmlContents/KeyWords");
  if (!kw_span.empty()) {
    auto sheets = internal::ParseKeywordsXml(kw_span);
    if (!sheets.empty()) {
      // Populate preview PNGs by matching sheet names to SheetPreviews index.
      auto names_span = internal::GetStream(streams, "SheetPreviews/SheetNames");
      if (!names_span.empty()) {
        auto names = internal::ParseSheetNames(names_span);
        // Build name→index map for O(1) lookup.
        std::map<std::string, std::size_t> name_to_idx;
        for (std::size_t i = 0; i < names.size(); ++i)
          name_to_idx.emplace(names[i], i);
        for (auto& sheet : sheets) {
          auto it = name_to_idx.find(sheet.name);
          if (it != name_to_idx.end())
            sheet.preview_png = GetPreviewPng(
                streams, "Images/Sheet_" + std::to_string(it->second));
        }
      }
      return sheets;
    }
  }

  // Fallback: build sheets from SheetPreviews/SheetNames (no view data).
  auto names_span = internal::GetStream(streams, "SheetPreviews/SheetNames");
  if (names_span.empty()) return {};

  auto names = internal::ParseSheetNames(names_span);
  std::vector<Sheet> sheets;
  sheets.reserve(names.size());
  for (std::size_t i = 0; i < names.size(); ++i) {
    Sheet sheet;
    sheet.name = std::move(names[i]);
    sheet.preview_png = GetPreviewPng(
        streams, "Images/Sheet_" + std::to_string(i));
    sheets.push_back(std::move(sheet));
  }
  return sheets;
}

}  // namespace

// ── SwxDocument::Open ─────────────────────────────────────────────────────────

Result<SwxDocument> SwxDocument::Open(const std::filesystem::path& path) {
  // Step 1: Read file into memory.
  auto file_result = ReadFile(path);
  if (!file_result.ok())
    return Result<SwxDocument>::Err(file_result.error());
  const std::vector<uint8_t>& file_data = file_result.value();

  // Step 2: Parse into stream map.
  auto stream_result =
      ParseFile(std::span<const uint8_t>(file_data));
  if (!stream_result.ok())
    return Result<SwxDocument>::Err(stream_result.error());
  StreamMap streams = std::move(stream_result).value();

  if (streams.empty())
    return Result<SwxDocument>::Err("No streams found in file: " +
                                    path.string());

  // Step 3: Build the Document.
  Document doc;
  doc.type    = InferDocumentType(path);
  doc.version = ExtractVersion(streams);

  // Step 4: Global properties.
  // Primary source: docProps/custom.xml (user-defined, takes precedence).
  auto custom_span = internal::GetStream(streams, "docProps/custom.xml");
  if (!custom_span.empty())
    doc.global_properties = internal::ParsePropertyXml(custom_span);

  // Supplement with OOXML core properties (Author, Title, LastSavedBy, …).
  // Only fills keys not already set by custom.xml.
  auto core_span = internal::GetStream(streams, "docProps/core.xml");
  if (!core_span.empty()) {
    auto core_props = internal::ParseCoreXml(core_span);
    for (auto& [k, v] : core_props)
      doc.global_properties.emplace(std::move(k), std::move(v));
  }

  // Step 5: Document preview.
  doc.preview_png = GetPreviewPng(streams, "PreviewPNG");

  if (doc.type == DocumentType::kDrawing) {
    // Step 6a: Drawing — build sheets.
    doc.sheets = BuildSheets(streams);
  } else {
    // Step 6b: Part / Assembly — build configurations.

    // Pre-parse CMgrHdr2 for config-name resolution (highest priority source).
    // Versions >= 2070 (0x0816) use "CMgrHdr2"; older files use "CMgrHdr".
    // We try CMgrHdr2 first and fall back to CMgrHdr — this is robust against
    // files that report an incorrect version number.
    // The document version is passed so that exactly the right optional fields
    // are read per config record (greedy reading desynchronises the parser).
    std::map<int, std::string> cmgrhdr2_names;
    {
      auto span = internal::GetStream(streams, "Contents/CMgrHdr2");
      if (span.empty())
        span = internal::GetStream(streams, "Contents/CMgrHdr");
      if (!span.empty())
        cmgrhdr2_names = internal::ParseCMgrHdr2(span, doc.version);
    }

    // Pre-parse COMPINSTANCETREE for config-name resolution (fallback source).
    std::map<int, std::string> compinstancetree_names;
    auto cit_span =
        internal::GetStream(streams, "swXmlContents/COMPINSTANCETREE");
    if (!cit_span.empty())
      compinstancetree_names = internal::ParseConfigNames(cit_span);

    std::set<int> indices = DiscoverConfigIndices(streams);

    // Augment with configIds from CMgrHdr2: configs that exist in the manager
    // header but have no corresponding per-config stream are still valid and
    // may carry properties via custom.xml.
    for (const auto& [cid, _] : cmgrhdr2_names)
      indices.insert(cid);

    // If no indices were found (very old file or no per-config streams), add 0.
    if (indices.empty()) indices.insert(0);

    doc.configurations.reserve(indices.size());
    for (int n : indices) {
      doc.configurations.push_back(BuildConfiguration(
          streams, n, compinstancetree_names, cmgrhdr2_names, doc.type));
    }

    // Deduplicate: SolidWorks stores display-state variants as separate config
    // indices that share the same configuration name.  Keep only the first
    // occurrence of each name (which has the lowest index = most canonical).
    // Unnamed configs ('') are kept as-is for later inference processing.
    {
      std::set<std::string> seen_names;
      auto it = doc.configurations.begin();
      while (it != doc.configurations.end()) {
        if (!it->name.empty()) {
          if (!seen_names.insert(it->name).second) {
            it = doc.configurations.erase(it);
            continue;
          }
        }
        ++it;
      }
    }

    // Post-process: if exactly one configuration has no name, try
    // ISolidWorksInformation.xml "SW-Configuration Name" as a fallback.
    // This handles files where the config index ≠ 0 (e.g. index=2) but the
    // file has only one configuration and it is the active one.
    {
      std::vector<Configuration*> unnamed;
      for (auto& cfg : doc.configurations)
        if (cfg.name.empty()) unnamed.push_back(&cfg);

      if (unnamed.size() == 1) {
        auto sw_info_span =
            internal::GetStream(streams, "docProps/ISolidWorksInformation.xml");
        if (!sw_info_span.empty()) {
          auto info = internal::ParseSwInformation(sw_info_span);
          auto it = info.find("SW-Configuration Name");
          if (it != info.end() && !it->second.empty()) {
            // Only assign if no other config already uses this name.
            bool name_taken = false;
            for (const auto& cfg : doc.configurations)
              if (!cfg.name.empty() && cfg.name == it->second) {
                name_taken = true;
                break;
              }
            if (!name_taken)
              unnamed[0]->name = it->second;
          }
        }
      }
    }

    // Post-process: infer base config names from "* <X>" or "*<X>" patterns.
    // SolidWorks auto-creates derived configs like "LANG <X>", "vercrimpt <X>",
    // or "schwarz<Standard>" whose base config is "X".  When exactly one config
    // is unnamed and exactly one unique base name can be inferred this way,
    // assign that base name to the unnamed config.
    {
      std::set<std::string> known_names;
      std::vector<std::string> inferable_bases;

      for (const auto& cfg : doc.configurations) {
        if (!cfg.name.empty()) known_names.insert(cfg.name);
      }
      for (const auto& cfg : doc.configurations) {
        if (cfg.name.size() < 3) continue;  // too short for "X<Y>"
        if (cfg.name.back() != '>') continue;
        auto lt = cfg.name.rfind('<');
        if (lt == std::string::npos || lt == 0) continue;

        bool has_space_before_lt = (cfg.name[lt - 1] == ' ');
        std::string base = cfg.name.substr(lt + 1, cfg.name.size() - lt - 2);
        if (base.empty()) continue;

        if (!has_space_before_lt) {
          // Pattern "X<Y>" (no space): Y is the base config only if Y contains
          // no spaces.  Compound phrases like "Wie bearbeitet" inside <> indicate
          // a state variant (the prefix before < is the base config), not a
          // derived-config reference.
          if (base.find(' ') != std::string::npos) continue;
        }

        if (!known_names.count(base))
          inferable_bases.push_back(std::move(base));
      }

      std::vector<Configuration*> unnamed;
      for (auto& cfg : doc.configurations)
        if (cfg.name.empty()) unnamed.push_back(&cfg);

      if (unnamed.size() == 1 && inferable_bases.size() == 1)
        unnamed[0]->name = inferable_bases[0];
    }
  }

  return Result<SwxDocument>::Ok(SwxDocument(std::move(doc)));
}

}  // namespace openswx
