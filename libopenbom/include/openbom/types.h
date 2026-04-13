// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

/// @file types.h
/// @brief Public data types for the openbom library.
///
/// All quantities use the same SI base units as the underlying openswx library.
/// Lengths are in metres, masses in kilograms, areas in m², volumes in m³.

#pragma once

#include <map>
#include <string>
#include <vector>

namespace openbom {

// ─────────────────────────────────────────────────────────────────────────────
// BomItemType
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Classifies what a BOM item represents.
enum class BomItemType {
  kPart,         ///< Leaf part file (.SLDPRT) without cut-list expansion.
  kAssembly,     ///< Assembly file (.SLDASM) with component children.
  kCutListItem,  ///< One entry from a weldment cut list (always a leaf).
  kDrawing,      ///< Drawing file (.SLDDRW); children are the referenced models.
};

// ─────────────────────────────────────────────────────────────────────────────
// BomItem
// ─────────────────────────────────────────────────────────────────────────────

/// @brief One row in the bill of materials tree.
///
/// A BomItem is either a leaf (part, cut-list item) or an inner node (assembly,
/// drawing) whose @ref children carry the sub-BOM.
///
/// **Quantity semantics**
/// - For assembly components the transformer aggregates identical instances
///   (same resolved path + configuration) into a single BomItem and sets
///   @ref quantity to the instance count.
/// - For cut-list items @ref quantity comes directly from the cut-list entry.
/// - For the root item of a directly opened file @ref quantity is always 1.
///
/// **Properties**
/// Configuration-level properties take precedence over file-level properties
/// when both define the same key (same merge logic as openswx).
struct BomItem {
  /// Instance name within the parent assembly, the cut-list feature name,
  /// the model name derived from the file stem, or the drawing file stem.
  std::string name;

  /// Resolved host-filesystem path of the source file (forward-slash
  /// separators).  Empty when the file could not be located.
  std::string file_path;

  /// Original Windows path as written by SolidWorks (backslash separators).
  /// May be empty for in-context components or the top-level document.
  std::string windows_path;

  /// Name of the active configuration (or cut-list configuration).
  std::string configuration;

  /// Number of identical instances within the parent assembly, or cut-list
  /// quantity.  Always 1 for the BOM root.
  int quantity = 1;

  BomItemType type = BomItemType::kPart;  ///< Item classification.

  bool is_suppressed     = false;  ///< True when component is suppressed.
  bool exclude_from_bom  = false;  ///< True when marked "exclude from BOM".

  /// Merged custom properties (file-level overridden by config-level).
  std::map<std::string, std::string> properties;

  /// Direct children: sub-assembly components, cut-list entries, or
  /// referenced drawing models.  Empty for leaf items.
  std::vector<BomItem> children;
};

// ─────────────────────────────────────────────────────────────────────────────
// Bom
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The complete bill of materials produced by @ref BomTransformer.
struct Bom {
  /// Root of the BOM tree, corresponding to the file passed to
  /// @ref BomTransformer::Build().
  BomItem root;

  /// Human-readable warnings accumulated during traversal: unresolved
  /// component paths, parse errors on sub-files, cycle detections, etc.
  std::vector<std::string> warnings;
};

}  // namespace openbom
