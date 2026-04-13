// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#include "sw/document_analyzer.h"

#include <span>

#include "openswx/document.h"
#include "utils/base64.h"

namespace sw_dumper::sw {

namespace {

using nlohmann::json;
using openswx::Configuration;
using openswx::Document;
using openswx::DocumentType;
using openswx::MassProperties;
using openswx::Sheet;

// doc_type integers expected by the HTML viewer and sqlite_storage.
constexpr int kDocTypePart     = 1;
constexpr int kDocTypeAssembly = 2;
constexpr int kDocTypeDrawing  = 3;

std::string PngToBase64(const std::vector<uint8_t>& png) {
  if (png.empty()) return {};
  return utils::Base64Encode(std::span<const uint8_t>(png));
}

json SerializeMassProperties(const MassProperties& mp) {
  return json{
      {"center_of_gravity",
       {{"x", mp.center_of_gravity[0]},
        {"y", mp.center_of_gravity[1]},
        {"z", mp.center_of_gravity[2]}}},
      {"volume",       mp.volume},
      {"surface_area", mp.surface_area},
      {"mass",         mp.mass},
      {"moments_of_inertia",
       {mp.moments_of_inertia[0], mp.moments_of_inertia[1],
        mp.moments_of_inertia[2]}},
      {"products_of_inertia",
       {mp.products_of_inertia[0], mp.products_of_inertia[1],
        mp.products_of_inertia[2]}},
  };
}

json SerializeConfiguration(const Configuration& cfg) {
  json jcfg;
  jcfg["name"]  = cfg.name;
  jcfg["index"] = cfg.index;

  jcfg["properties"] = json::object();
  for (const auto& [k, v] : cfg.properties)
    jcfg["properties"][k] = v;

  if (cfg.mass_properties)
    jcfg["mass_properties"] = SerializeMassProperties(*cfg.mass_properties);

  std::string png_b64 = PngToBase64(cfg.preview_png);
  if (!png_b64.empty())
    jcfg["preview_png_base64"] = std::move(png_b64);

  json jcl = json::array();
  for (const auto& item : cfg.cut_list) {
    json ji;
    ji["name"]       = item.name;
    ji["quantity"]   = item.quantity;
    ji["properties"] = json::object();
    for (const auto& [k, v] : item.properties)
      ji["properties"][k] = v;
    jcl.push_back(std::move(ji));
  }
  jcfg["cut_list"] = std::move(jcl);

  json jcomps = json::array();
  for (const auto& c : cfg.components) {
    jcomps.push_back({
        {"name",                c.name},
        {"path",                c.path},
        {"ref_config",          c.configuration_name},
        {"component_reference", c.component_reference},
        {"is_suppressed",       c.is_suppressed},
        {"is_hidden",           c.is_hidden},
        {"exclude_from_bom",    c.exclude_from_bom},
    });
  }
  jcfg["components"] = std::move(jcomps);

  return jcfg;
}

json SerializeDocument(const Document& doc) {
  json j;

  switch (doc.type) {
    case DocumentType::kPart:     j["doc_type"] = kDocTypePart;     break;
    case DocumentType::kAssembly: j["doc_type"] = kDocTypeAssembly; break;
    case DocumentType::kDrawing:  j["doc_type"] = kDocTypeDrawing;  break;
  }

  j["version"] = doc.version;

  std::string doc_png = PngToBase64(doc.preview_png);
  if (!doc_png.empty())
    j["preview_png_base64"] = std::move(doc_png);

  j["global_properties"] = json::object();
  for (const auto& [k, v] : doc.global_properties)
    j["global_properties"][k] = v;

  j["configurations"] = json::array();
  for (const auto& cfg : doc.configurations)
    j["configurations"].push_back(SerializeConfiguration(cfg));

  j["sheets"] = json::array();
  for (const auto& sheet : doc.sheets) {
    json js;
    js["name"]    = sheet.name;
    js["views"]   = json::array();
    std::string s_png = PngToBase64(sheet.preview_png);
    if (!s_png.empty())
      js["preview_png_base64"] = std::move(s_png);
    for (const auto& v : sheet.views) {
      js["views"].push_back({
          {"name",                 v.name},
          {"referenced_document",  v.referenced_document},
          {"referenced_config",    v.referenced_configuration},
      });
    }
    j["sheets"].push_back(std::move(js));
  }

  return j;
}

}  // namespace

bool DocumentAnalyzer::AnalyzeFile(const std::filesystem::path& path,
                                    nlohmann::json* data) const {
  auto result = openswx::SwxDocument::Open(path);
  if (!result.ok()) return false;
  *data = SerializeDocument(result.value().doc());
  return true;
}

}  // namespace sw_dumper::sw
