// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#pragma once

// internal/xml_readers.h — Parsers for SolidWorks XML streams.
//
// Each function accepts the raw UTF-8 bytes of the respective XML stream
// and returns the extracted data.  Returns empty/nullopt on parse error.

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "openswx/types.h"

namespace openswx::internal {

// ── docProps/custom.xml and docProps/Config-N-Properties.xml ──────────────
//
// Both formats share the same schema: <property name="..."><vt:lpstr>...</>
// within a <propertySection name="UserDefinedProperties"> element.
// Returns a map of non-empty property names to their string values.
[[nodiscard]] std::map<std::string, std::string> ParsePropertyXml(
    std::span<const uint8_t> xml_bytes);

// ── docProps/ISolidWorksInformation.xml ──────────────────────────────────────
//
// Same schema as ParsePropertyXml.  Additionally extracts the cached mass
// properties for the given config index from the "SW-MassProp-Config-N"
// property.  Returns nullopt if the property is absent or malformed.
[[nodiscard]] std::optional<MassProperties> ParseMassProperties(
    std::span<const uint8_t> xml_bytes, int config_index);

// Extracts all SW system properties (SW-Dateiname, SW-Configuration Name,
// SW-Last Saved By, …) from ISolidWorksInformation.xml.
[[nodiscard]] std::map<std::string, std::string> ParseSwInformation(
    std::span<const uint8_t> xml_bytes);

// ── docProps/Config-N-Cutlist-Properties.xml ─────────────────────────────────
[[nodiscard]] std::vector<CutListItem> ParseCutlistXml(
    std::span<const uint8_t> xml_bytes);

// ── swXmlContents/COMPINSTANCETREE ───────────────────────────────────────────
//
// Returns the components for a given configuration index.
// For assemblies: the <swReference> children of the assembly model with the
// matching swConfigurationId.
// For parts: returns an empty vector (no components).
[[nodiscard]] std::vector<Component> ParseComponents(
    std::span<const uint8_t> xml_bytes, int config_index);

// Returns all (swConfigurationId → swConfigurationName) mappings found in
// the COMPINSTANCETREE for the document's own models (i.e. <swModel> elements
// that belong to the file being parsed).
[[nodiscard]] std::map<int, std::string> ParseConfigNames(
    std::span<const uint8_t> xml_bytes);

// ── docProps/core.xml ─────────────────────────────────────────────────────────
//
// Standard OOXML core properties (Dublin Core).
// Returns a map with English property names:
//   Author, LastSavedBy, Title, Subject, Keywords, Comments,
//   CreateDateTime, LastSaveDateTime.
// Values are taken as-is from the file (dates in ISO 8601 / UTC).
[[nodiscard]] std::map<std::string, std::string> ParseCoreXml(
    std::span<const uint8_t> xml_bytes);

// ── swXmlContents/KeyWords ────────────────────────────────────────────────────
//
// Extracts drawing sheets and their views.
// Only <Sheet Type="Sheet"> nodes are included (not "Sheet Format" nodes).
// Each view carries: name, referenced_document (text content, trimmed),
// referenced_configuration (Description attribute).
[[nodiscard]] std::vector<Sheet> ParseKeywordsXml(
    std::span<const uint8_t> xml_bytes);

}  // namespace openswx::internal
