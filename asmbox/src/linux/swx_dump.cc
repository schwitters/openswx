// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

// swx_dump.cc — Linux CLI tool that demonstrates the openswx library.
//
// Usage: swx_dump <file.SLDPRT|file.SLDASM|file.SLDDRW>
//
// Prints a JSON representation of the parsed document to stdout.
// The JSON schema is intentionally compatible with the Windows sw_dumper
// output so that downstream consumers can be shared.

#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "openswx/document.h"

namespace {

using nlohmann::json;
using openswx::Configuration;
using openswx::Document;
using openswx::DocumentType;
using openswx::MassProperties;
using openswx::Sheet;

json SerializeMassProperties(const MassProperties& mp) {
  return json{
      {"center_of_gravity",
       {{"x", mp.center_of_gravity[0]},
        {"y", mp.center_of_gravity[1]},
        {"z", mp.center_of_gravity[2]}}},
      {"volume", mp.volume},
      {"surface_area", mp.surface_area},
      {"mass", mp.mass},
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

  // Properties.
  jcfg["properties"] = json::object();
  for (const auto& [k, v] : cfg.properties) jcfg["properties"][k] = v;

  // Mass properties.
  if (cfg.mass_properties)
    jcfg["mass_properties"] = SerializeMassProperties(*cfg.mass_properties);

  // Preview PNG as base64.
  if (!cfg.preview_png.empty()) {
    static constexpr char kB64Chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto& png = cfg.preview_png;
    std::string b64;
    b64.reserve(((png.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < png.size(); i += 3) {
      uint32_t grp = static_cast<uint32_t>(png[i]) << 16;
      if (i + 1 < png.size()) grp |= static_cast<uint32_t>(png[i + 1]) << 8;
      if (i + 2 < png.size()) grp |= static_cast<uint32_t>(png[i + 2]);
      b64 += kB64Chars[(grp >> 18) & 0x3F];
      b64 += kB64Chars[(grp >> 12) & 0x3F];
      b64 += (i + 1 < png.size()) ? kB64Chars[(grp >> 6) & 0x3F] : '=';
      b64 += (i + 2 < png.size()) ? kB64Chars[grp & 0x3F] : '=';
    }
    jcfg["preview_png_base64"] = std::move(b64);
  }

  // Cut list.
  json jcl = json::array();
  for (const auto& item : cfg.cut_list) {
    json ji;
    ji["name"]     = item.name;
    ji["quantity"] = item.quantity;
    ji["properties"] = json::object();
    for (const auto& [k, v] : item.properties) ji["properties"][k] = v;
    jcl.push_back(std::move(ji));
  }
  jcfg["cut_list"] = std::move(jcl);

  // Components (assemblies).
  json jcomps = json::array();
  for (const auto& c : cfg.components) {
    jcomps.push_back({
        {"name", c.name},
        {"path", c.path},
        {"ref_config", c.configuration_name},
        {"component_reference", c.component_reference},
        {"is_suppressed", c.is_suppressed},
        {"is_hidden", c.is_hidden},
        {"exclude_from_bom", c.exclude_from_bom},
    });
  }
  jcfg["components"] = std::move(jcomps);

  return jcfg;
}

json SerializeDocument(const Document& doc) {
  json j;

  switch (doc.type) {
    case DocumentType::kPart:     j["doc_type"] = "part";     break;
    case DocumentType::kAssembly: j["doc_type"] = "assembly"; break;
    case DocumentType::kDrawing:  j["doc_type"] = "drawing";  break;
  }

  j["version"] = doc.version;

  j["global_properties"] = json::object();
  for (const auto& [k, v] : doc.global_properties)
    j["global_properties"][k] = v;

  j["configurations"] = json::array();
  for (const auto& cfg : doc.configurations)
    j["configurations"].push_back(SerializeConfiguration(cfg));

  j["sheets"] = json::array();
  for (const auto& sheet : doc.sheets) {
    json js;
    js["name"] = sheet.name;
    js["views"] = json::array();
    for (const auto& v : sheet.views) {
      js["views"].push_back({
          {"name", v.name},
          {"referenced_document", v.referenced_document},
          {"referenced_config", v.referenced_configuration},
      });
    }
    j["sheets"].push_back(std::move(js));
  }

  return j;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: swx_dump <SolidWorks file>\n";
    return 1;
  }

  auto result = openswx::SwxDocument::Open(argv[1]);
  if (!result.ok()) {
    std::cerr << "Error: " << result.error() << "\n";
    return 1;
  }

  std::cout << SerializeDocument(result.value().doc()).dump(2) << "\n";
  return 0;
}
