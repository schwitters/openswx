// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#include "internal/xml_readers.h"

#include <charconv>
#include <sstream>
#include <string_view>

#include <pugixml.hpp>

namespace openswx::internal {

namespace {

// ── Shared XML utilities ─────────────────────────────────────────────────────

// Loads xml_bytes into a pugi document.  Returns false on parse error.
bool LoadXml(const std::span<const uint8_t>& xml_bytes,
             pugi::xml_document& doc) {
  if (xml_bytes.empty()) return false;
  auto result = doc.load_buffer(
      xml_bytes.data(), xml_bytes.size(),
      pugi::parse_default | pugi::parse_trim_pcdata);
  return result.status == pugi::status_ok;
}

// Returns the text content of the first supported vt:* value child of node.
// Handles vt:lpstr, vt:i4, vt:i2, vt:r8, vt:bool, vt:date.
std::string ExtractVtValue(const pugi::xml_node& prop) {
  for (const char* tag :
       {"vt:lpstr", "vt:lpwstr", "vt:i4", "vt:i2", "vt:ui4", "vt:ui2",
        "vt:r8", "vt:bool", "vt:date", "vt:filetime"}) {
    pugi::xml_node child = prop.child(tag);
    if (child) return child.child_value();
  }
  return {};
}

// Extracts all <property name="..."> entries from a single
// <propertySection> node into the output map.
void ExtractSection(const pugi::xml_node& section,
                    std::map<std::string, std::string>& out) {
  for (pugi::xml_node prop : section.children("property")) {
    std::string name = prop.attribute("name").as_string();
    if (name.empty()) continue;
    std::string value = ExtractVtValue(prop);
    // First definition wins (preserve ordering as in file).
    out.emplace(std::move(name), std::move(value));
  }
}

// Parses all <propertySection> children of root into a flat map.
std::map<std::string, std::string> ExtractAllSections(
    const pugi::xml_node& root) {
  std::map<std::string, std::string> out;
  for (pugi::xml_node sec : root.children("propertySection")) {
    std::string_view sec_name = sec.attribute("name").as_string();
    // Only extract user-defined properties (not DocumentSummaryInformation).
    if (sec_name == "UserDefinedProperties") {
      ExtractSection(sec, out);
    }
  }
  return out;
}

// Parses the comma-separated double string produced by SW for mass properties.
// Format: 14 doubles separated by ", " (see format-spec.md §7.3).
std::optional<MassProperties> ParseMassPropString(std::string_view str) {
  std::array<double, 14> vals{};
  std::size_t idx = 0;
  std::size_t pos = 0;

  while (idx < 14 && pos < str.size()) {
    // Skip whitespace and commas.
    while (pos < str.size() &&
           (str[pos] == ' ' || str[pos] == ',')) {
      ++pos;
    }
    if (pos >= str.size()) break;

    // Find end of token.
    std::size_t end = str.find_first_of(", ", pos);
    std::string_view token =
        str.substr(pos, end == std::string_view::npos ? end : end - pos);

    double v = 0.0;
    auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(),
                                     v, std::chars_format::general);
    if (ec != std::errc{}) return std::nullopt;
    vals[idx++] = v;
    pos = (end == std::string_view::npos) ? str.size() : end;
  }

  if (idx < 12) return std::nullopt;  // need at least CoG+vol+surf+mass+inertia

  MassProperties mp;
  mp.center_of_gravity = {vals[0], vals[1], vals[2]};
  mp.volume            = vals[3];
  mp.surface_area      = vals[4];
  mp.mass              = vals[5];
  mp.moments_of_inertia  = {vals[6], vals[7], vals[8]};
  mp.products_of_inertia = {vals[9], vals[10], vals[11]};
  return mp;
}

// Converts "NO"/"YES"/"TRUE"/"FALSE" strings to bool.
bool ParseBoolAttr(const pugi::xml_attribute& attr) noexcept {
  std::string_view v = attr.as_string();
  return v == "YES" || v == "TRUE" || v == "yes" || v == "true";
}


}  // namespace

// ── Public implementations ───────────────────────────────────────────────────

std::map<std::string, std::string> ParsePropertyXml(
    std::span<const uint8_t> xml_bytes) {
  pugi::xml_document doc;
  if (!LoadXml(xml_bytes, doc)) return {};
  pugi::xml_node root = doc.document_element();
  return ExtractAllSections(root);
}

std::optional<MassProperties> ParseMassProperties(
    std::span<const uint8_t> xml_bytes, int config_index) {
  pugi::xml_document doc;
  if (!LoadXml(xml_bytes, doc)) return std::nullopt;

  // Build the property name: "SW-MassProp-Config-N".
  std::string target = "SW-MassProp-Config-" + std::to_string(config_index);

  pugi::xml_node root = doc.document_element();
  for (pugi::xml_node sec : root.children("propertySection")) {
    for (pugi::xml_node prop : sec.children("property")) {
      if (target == prop.attribute("name").as_string()) {
        std::string val_str = ExtractVtValue(prop);
        return ParseMassPropString(val_str);
      }
    }
  }
  return std::nullopt;
}

std::map<std::string, std::string> ParseSwInformation(
    std::span<const uint8_t> xml_bytes) {
  pugi::xml_document doc;
  if (!LoadXml(xml_bytes, doc)) return {};
  // ISolidWorksInformation.xml uses the same schema; take all sections.
  pugi::xml_node root = doc.document_element();
  std::map<std::string, std::string> out;
  for (pugi::xml_node sec : root.children("propertySection")) {
    ExtractSection(sec, out);
  }
  return out;
}

std::vector<CutListItem> ParseCutlistXml(std::span<const uint8_t> xml_bytes) {
  pugi::xml_document doc;
  if (!LoadXml(xml_bytes, doc)) return {};

  std::vector<CutListItem> items;
  pugi::xml_node config = doc.document_element();  // <Configuration>

  for (pugi::xml_node feat : config.children("Feature")) {
    CutListItem item;
    item.name = feat.attribute("Name").as_string();

    for (pugi::xml_node prop : feat.children("CustomProperty")) {
      std::string prop_name = prop.attribute("Name").as_string();
      if (prop_name.empty()) continue;

      // The text content of <CustomProperty> is the resolved value.
      std::string value = prop.child_value();

      if (prop_name == "QUANTITY") {
        int qty = 0;
        auto [ptr, ec] =
            std::from_chars(value.data(), value.data() + value.size(), qty);
        if (ec == std::errc{}) item.quantity = qty;
      }
      item.properties.emplace(std::move(prop_name), std::move(value));
    }

    // Fallback: read <Quantity> element if QUANTITY property is missing.
    if (item.quantity == 0) {
      pugi::xml_node qty_node = feat.child("Quantity");
      if (qty_node) {
        int qty = 0;
        std::string_view qv = qty_node.child_value();
        std::from_chars(qv.data(), qv.data() + qv.size(), qty);
        item.quantity = qty;
      }
    }

    items.push_back(std::move(item));
  }

  return items;
}

std::vector<Component> ParseComponents(std::span<const uint8_t> xml_bytes,
                                        int config_index) {
  pugi::xml_document doc;
  if (!LoadXml(xml_bytes, doc)) return {};

  pugi::xml_node sw_root = doc.document_element();  // <swSolidWorks>

  // Build file-id → path map from <swHeader>/<swFile> elements.
  // Also record the ID of the first file entry — that is the document itself
  // (the top-level assembly whose components we want to enumerate).
  std::map<std::string, std::string> file_paths;
  std::string top_file_id;  // ID of the document's own swFile entry.
  for (pugi::xml_node f : sw_root.child("swHeader").children("swFile")) {
    std::string id   = f.attribute("id").as_string();
    std::string path = f.attribute("swPath").as_string();
    if (!id.empty()) {
      if (top_file_id.empty()) top_file_id = id;  // first entry = this file
      file_paths.emplace(std::move(id), std::move(path));
    }
  }

  // Build model-id → {fileRef, configName} maps from <swModelList>/<swModel>.
  std::map<std::string, std::string> model_file_refs;
  std::map<std::string, std::string> model_config_names;
  for (pugi::xml_node m :
       sw_root.child("swModelList").children("swModel")) {
    std::string id      = m.attribute("id").as_string();
    std::string fileref = m.attribute("swFileRef").as_string();
    std::string cfgname = m.attribute("swConfigurationName").as_string();
    if (!id.empty()) {
      model_file_refs.emplace(id, std::move(fileref));
      if (!cfgname.empty())
        model_config_names.emplace(std::move(id), std::move(cfgname));
    }
  }

  // Find the <swModel> element for the requested configuration index.
  // The correct model is the one that:
  //   1. Has swConfigurationId == config_index, AND
  //   2. Has at least one <swReference> child (i.e. is an assembly config), AND
  //   3. Has swFileRef pointing to the top-level file (the document itself).
  //
  // Condition 3 avoids mistakenly processing a sub-assembly model that appears
  // earlier in the list but shares the same configuration index.
  std::string cfg_id_str = std::to_string(config_index);

  std::vector<Component> components;

  for (pugi::xml_node m :
       sw_root.child("swModelList").children("swModel")) {
    if (cfg_id_str != m.attribute("swConfigurationId").as_string()) continue;
    if (!m.child("swReference")) continue;  // not an assembly model
    // Skip sub-assembly models: their swFileRef differs from the top-level file.
    if (!top_file_id.empty() &&
        top_file_id != m.attribute("swFileRef").as_string()) continue;

    for (pugi::xml_node ref : m.children("swReference")) {
      Component comp;
      comp.name = ref.attribute("swName").as_string();

      // An abbreviated swReference (missing swSuppressed) encodes a suppressed
      // component.  SolidWorks omits the full attribute set for these entries
      // rather than repeating large XML for inactive components.
      if (ref.attribute("swSuppressed").empty()) {
        comp.is_suppressed    = true;
        comp.is_hidden        = true;
        comp.exclude_from_bom = true;
      } else {
        comp.is_suppressed    = ParseBoolAttr(ref.attribute("swSuppressed"));
        comp.is_hidden        = ParseBoolAttr(ref.attribute("swHidden"));
        comp.exclude_from_bom = ParseBoolAttr(ref.attribute("swExcludeFromBOM"));
      }

      comp.configuration_name =
          ref.attribute("swConfigurationName").as_string();
      comp.component_reference =
          ref.attribute("swComponentReference").as_string();

      // Resolve the file path: swReference → swModel → swFile.
      std::string model_ref = ref.attribute("swModelRef").as_string();
      auto it_m = model_file_refs.find(model_ref);
      if (it_m != model_file_refs.end()) {
        auto it_f = file_paths.find(it_m->second);
        if (it_f != file_paths.end()) {
          comp.path = it_f->second;  // Store original Windows path.
        }
      }

      // For suppressed (abbreviated) records, the configuration_name attribute
      // is absent.  Fall back to the component model's own configuration name.
      if (comp.configuration_name.empty()) {
        auto it_cfg = model_config_names.find(model_ref);
        if (it_cfg != model_config_names.end())
          comp.configuration_name = it_cfg->second;
      }

      components.push_back(std::move(comp));
    }
    break;  // Found the top-level assembly config; no need to continue.
  }

  return components;
}

std::map<int, std::string> ParseConfigNames(
    std::span<const uint8_t> xml_bytes) {
  pugi::xml_document doc;
  if (!LoadXml(xml_bytes, doc)) return {};

  pugi::xml_node sw_root = doc.document_element();
  std::map<int, std::string> result;

  for (pugi::xml_node m :
       sw_root.child("swModelList").children("swModel")) {
    std::string_view cfg_id_str = m.attribute("swConfigurationId").as_string();
    std::string_view cfg_name   = m.attribute("swConfigurationName").as_string();
    if (cfg_id_str.empty() || cfg_name.empty()) continue;

    int cfg_id = 0;
    auto [ptr, ec] = std::from_chars(
        cfg_id_str.data(), cfg_id_str.data() + cfg_id_str.size(), cfg_id);
    if (ec == std::errc{}) result.emplace(cfg_id, std::string(cfg_name));
  }

  return result;
}

std::map<std::string, std::string> ParseCoreXml(
    std::span<const uint8_t> xml_bytes) {
  pugi::xml_document doc;
  if (!LoadXml(xml_bytes, doc)) return {};

  pugi::xml_node root = doc.document_element();  // <cp:coreProperties>

  // Dublin Core element name → output property key.
  static constexpr std::pair<const char*, const char*> kMappings[] = {
      {"dc:creator",        "Author"},
      {"dc:lastModifiedBy", "LastSavedBy"},
      {"dc:title",          "Title"},
      {"dc:subject",        "Subject"},
      {"dc:keywords",       "Keywords"},
      {"dc:description",    "Comments"},
      {"dcterms:created",   "CreateDateTime"},
      {"dcterms:modified",  "LastSaveDateTime"},
  };

  std::map<std::string, std::string> out;
  for (const auto& [elem, key] : kMappings) {
    pugi::xml_node node = root.child(elem);
    if (!node) continue;
    std::string val = node.child_value();
    if (!val.empty())
      out.emplace(key, std::move(val));
  }
  return out;
}

std::vector<Sheet> ParseKeywordsXml(std::span<const uint8_t> xml_bytes) {
  // The KeyWords stream sometimes has a leading non-XML byte before <?xml …>.
  // Skip any bytes before the first '<'.
  while (!xml_bytes.empty() && xml_bytes.front() != '<')
    xml_bytes = xml_bytes.subspan(1);

  pugi::xml_document doc;
  if (!LoadXml(xml_bytes, doc)) return {};

  pugi::xml_node root = doc.document_element();  // <Keywords>

  std::vector<Sheet> sheets;
  for (pugi::xml_node snode : root.children("Sheet")) {
    std::string_view type = snode.attribute("Type").as_string();
    // Accept "Sheet" (EN) and "Blatt" (DE); skip format/template nodes.
    if (type != "Sheet" && type != "Blatt") continue;

    Sheet sheet;
    sheet.name = snode.attribute("Name").as_string();

    for (pugi::xml_node vnode : snode.children("View")) {
      SheetView view;
      view.name = vnode.attribute("Name").as_string();
      view.referenced_configuration = vnode.attribute("Description").as_string();

      // Text content is the referenced document filename; trim whitespace.
      std::string raw = vnode.child_value();
      std::size_t s = raw.find_first_not_of(" \t\r\n");
      std::size_t e = raw.find_last_not_of(" \t\r\n");
      if (s != std::string::npos)
        view.referenced_document = raw.substr(s, e - s + 1);

      sheet.views.push_back(std::move(view));
    }

    sheets.push_back(std::move(sheet));
  }

  return sheets;
}

}  // namespace openswx::internal
