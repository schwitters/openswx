// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

/// @file types.h
/// @brief Public data types for the openswx library.
///
/// All physical quantities use SI base units unless stated otherwise:
/// | Quantity | Unit         |
/// |----------|--------------|
/// | Length   | metres (m)   |
/// | Area     | m²           |
/// | Volume   | m³           |
/// | Mass     | kg           |
/// | Inertia  | kg·m²        |

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace openswx {

// ─────────────────────────────────────────────────────────────────────────────
// DocumentType
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Type of a SolidWorks document, inferred from the file extension.
enum class DocumentType {
  kPart,      ///< Part file (.SLDPRT)
  kAssembly,  ///< Assembly file (.SLDASM)
  kDrawing,   ///< Drawing file (.SLDDRW)
};

// ─────────────────────────────────────────────────────────────────────────────
// MassProperties
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Precomputed mass properties cached by SolidWorks at save time.
///
/// SolidWorks evaluates mass properties during the save operation and stores
/// the results as a comma-separated string in
/// `docProps/ISolidWorksInformation.xml` under the property key
/// `SW-MassProp-Config-N` (where N is the configuration index).
///
/// All values are expressed in SI base units (metres, kilograms) and refer to
/// the model's output coordinate system.
struct MassProperties {
  /// Centre of gravity [x, y, z] in metres.
  std::array<double, 3> center_of_gravity{};

  double volume{};        ///< Total volume in m³.
  double surface_area{};  ///< Total surface area in m².
  double mass{};          ///< Total mass in kg.

  /// Principal moments of inertia [Ixx, Iyy, Izz] about axes through the
  /// centre of gravity, in kg·m².
  std::array<double, 3> moments_of_inertia{};

  /// Products of inertia [Ixy, Ixz, Iyz] at the centre of gravity,
  /// in kg·m².
  std::array<double, 3> products_of_inertia{};
};

// ─────────────────────────────────────────────────────────────────────────────
// CutListItem
// ─────────────────────────────────────────────────────────────────────────────

/// @brief One item in a weldment cut list.
///
/// Cut lists describe the individual structural members (tubes, plates, etc.)
/// in a weldment part.  Each item corresponds to one `<Feature>` element in
/// `docProps/Config-N-Cutlist-Properties.xml`.
struct CutListItem {
  /// Feature name as stored in the file (e.g. `"UNIDAL AA 7019<1>"`).
  std::string name;

  /// Number of identical bodies in the weldment.
  int quantity{};

  /// All custom properties attached to this cut-list item, keyed by property
  /// name.  The `QUANTITY` key is also present here when stored explicitly.
  std::map<std::string, std::string> properties;
};

// ─────────────────────────────────────────────────────────────────────────────
// Component
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A component instance within an assembly configuration.
///
/// Populated only for @ref DocumentType::kAssembly documents.
/// Source stream: `swXmlContents/COMPINSTANCETREE`.
///
/// @note The @ref path field stores the original Windows path as written by
///       SolidWorks and is not normalised to the host filesystem.
struct Component {
  /// Instance name within the assembly (e.g. `"Schraube-1"`).
  std::string name;

  /// Absolute Windows path of the referenced document as stored in the file.
  /// May be empty for in-context components or suppressed instances that lack
  /// a file reference.
  std::string path;

  /// Name of the referenced configuration inside the child document.
  std::string configuration_name;

  /// User-defined component reference annotation (may be empty).
  std::string component_reference;

  bool is_suppressed{};     ///< True if the component is suppressed.
  bool is_hidden{};         ///< True if the component is hidden in the viewport.
  bool exclude_from_bom{};  ///< True if excluded from bill-of-materials.
};

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A single SolidWorks configuration (part or assembly).
///
/// The library resolves configuration names from several internal sources in
/// priority order:
///  1. `Contents/CMgrHdr2` binary stream (most authoritative).
///  2. `swXmlContents/COMPINSTANCETREE` `@swConfigurationName` attribute.
///  3. `<Configuration Name="...">` in the cut-list XML.
///  4. `SW-Configuration Name` property in `docProps/ISolidWorksInformation.xml`.
///  5. Empty string when the name cannot be determined.
struct Configuration {
  /// Human-readable configuration name; empty when not determinable.
  std::string name;

  /// Zero-based configuration index N from the `Config-N-*` stream names.
  int index{};

  /// User-defined custom properties for this configuration.
  ///
  /// Built by merging file-level properties (`docProps/custom.xml`) with
  /// configuration-level properties (`docProps/Config-N-Properties.xml`).
  /// Configuration-level values take precedence over file-level values when
  /// both define the same key.
  std::map<std::string, std::string> properties;

  /// Precomputed mass properties, present only when SolidWorks stored them at
  /// save time.  Absent for configurations that have never been evaluated.
  std::optional<MassProperties> mass_properties;

  /// Raw PNG bytes of the configuration preview thumbnail.
  /// Empty when no preview is available.
  std::vector<uint8_t> preview_png;

  /// Cut-list items (weldment part files only).
  /// Empty for ordinary parts and all assemblies.
  std::vector<CutListItem> cut_list;

  /// Component instances (assembly files only).
  /// Empty for parts and drawings.
  std::vector<Component> components;
};

// ─────────────────────────────────────────────────────────────────────────────
// SheetView / Sheet
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A model view placed on a drawing sheet.
///
/// Populated from the `swXmlContents/KeyWords` stream.
/// The @ref referenced_document field stores the original Windows path as
/// written by SolidWorks.
struct SheetView {
  /// View name as shown in the SolidWorks feature tree.
  std::string name;

  /// Absolute Windows path of the source document for this view.
  std::string referenced_document;

  /// Name of the source configuration shown in this view.
  std::string referenced_configuration;
};

/// @brief A single sheet within a drawing document.
///
/// Populated only for @ref DocumentType::kDrawing documents.
/// View details (referenced documents and configurations) are sourced from
/// `swXmlContents/KeyWords`; preview images from `Images/Sheet_N`.
struct Sheet {
  /// Sheet name as shown in the SolidWorks tab bar.
  std::string name;

  /// Raw PNG bytes of the sheet thumbnail.  Empty when not available.
  std::vector<uint8_t> preview_png;

  /// All model views placed on this sheet.
  /// May be empty when the source stream is absent.
  std::vector<SheetView> views;
};

// ─────────────────────────────────────────────────────────────────────────────
// Document
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Top-level representation of a parsed SolidWorks document.
///
/// Obtain a @c Document by calling @ref SwxDocument::Open() and then
/// @ref SwxDocument::doc().
struct Document {
  /// Type of the document, derived from the file extension.
  DocumentType type{DocumentType::kPart};

  /// Internal model version number extracted from the `_MO_VERSION_NNNNN`
  /// stream-name prefix.
  ///
  /// This is the raw decimal integer used by SolidWorks internally, **not** a
  /// marketing release year.  Known reference points:
  /// | Decimal | SolidWorks release |
  /// |---------|--------------------|
  /// |   2070  | 2015               |
  /// |   3928  | ~2016              |
  /// |  11142  | ~2020              |
  /// |  12155  | ~2021              |
  ///
  /// Zero when the version stream is absent (e.g. very old or corrupt files).
  int version{};

  /// File-level custom properties shared across all configurations.
  ///
  /// Source: `docProps/custom.xml` (user-defined section) and
  /// `docProps/core.xml` (Dublin Core metadata such as `Author`,
  /// `CreateDateTime`, `LastSavedBy`).
  std::map<std::string, std::string> global_properties;

  /// Raw PNG bytes of the document-level preview thumbnail.
  /// Empty when no preview is stored in the file.
  std::vector<uint8_t> preview_png;

  /// One entry per configuration, ordered by ascending Config-N index.
  ///
  /// Display-state variants that share a configuration name with an earlier
  /// entry are deduplicated (only the lowest-index entry is kept).
  /// Always contains at least one entry for successfully parsed part and
  /// assembly files.  Empty for drawing files.
  std::vector<Configuration> configurations;

  /// Drawing sheets in order of appearance.
  /// Always empty for part and assembly files.
  std::vector<Sheet> sheets;
};

}  // namespace openswx
