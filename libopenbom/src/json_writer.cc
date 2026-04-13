// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#include "openbom/json_writer.h"

#include <fstream>

#include <nlohmann/json.hpp>

namespace openbom {

namespace {

// Convert BomItemType to a JSON-friendly string.
std::string TypeToString(BomItemType t) {
  switch (t) {
    case BomItemType::kPart:        return "part";
    case BomItemType::kAssembly:    return "assembly";
    case BomItemType::kCutListItem: return "cut_list_item";
    case BomItemType::kDrawing:     return "drawing";
  }
  return "unknown";
}

// Recursively serialise a BomItem to a nlohmann::json object.
nlohmann::json ItemToJson(const BomItem& item) {
  nlohmann::json j;
  j["name"]             = item.name;
  j["type"]             = TypeToString(item.type);
  j["quantity"]         = item.quantity;
  j["configuration"]    = item.configuration;
  j["file_path"]        = item.file_path;
  j["windows_path"]     = item.windows_path;
  j["is_suppressed"]    = item.is_suppressed;
  j["exclude_from_bom"] = item.exclude_from_bom;

  // Properties as a JSON object (always present, may be empty).
  j["properties"] = nlohmann::json::object();
  for (const auto& [k, v] : item.properties) j["properties"][k] = v;

  // Children array (always present, may be empty).
  j["children"] = nlohmann::json::array();
  for (const auto& child : item.children)
    j["children"].push_back(ItemToJson(child));

  return j;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// JsonWriter
// ─────────────────────────────────────────────────────────────────────────────

std::string JsonWriter::Write(const Bom& bom) const {
  nlohmann::json root;
  root["bom"]      = ItemToJson(bom.root);

  root["warnings"] = nlohmann::json::array();
  for (const auto& w : bom.warnings) root["warnings"].push_back(w);

  return pretty_ ? root.dump(2) : root.dump();
}

bool JsonWriter::WriteToFile(const Bom& bom,
                             const std::filesystem::path& path) const {
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) return false;
  out << Write(bom);
  return out.good();
}

}  // namespace openbom
