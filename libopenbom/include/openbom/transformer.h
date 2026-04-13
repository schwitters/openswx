// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

/// @file transformer.h
/// @brief Configurable BOM transformer built on top of libopenswx.
///
/// **Supported entry-point file types**
/// | Type      | Extension  | Behaviour                                      |
/// |-----------|------------|------------------------------------------------|
/// | Part      | .SLDPRT    | Single BOM root; cut-list items as children.   |
/// | Assembly  | .SLDASM    | Recursive component expansion.                 |
/// | Drawing   | .SLDDRW    | Extracts model/config from sheet views.        |
///
/// **Quick start**
/// @code{.cpp}
/// openbom::BomTransformerConfig cfg;
/// cfg.path_resolver.substitutions.push_back(
///     {"C:\\\\Company\\\\Parts\\\\", "/mnt/nas/parts/"});
///
/// openbom::BomTransformer transformer(cfg);
/// auto result = transformer.Build("/data/top_asm.SLDASM");
/// if (!result.ok()) { std::cerr << result.error() << "\n"; return 1; }
///
/// openbom::JsonWriter writer;
/// writer.WriteToFile(result.value(), "bom.json");
/// @endcode

#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "openswx/document.h"
#include "openbom/types.h"

namespace openbom {

// ─────────────────────────────────────────────────────────────────────────────
// PathResolverConfig
// ─────────────────────────────────────────────────────────────────────────────

/// @brief One prefix-replacement rule for Windows → host path mapping.
///
/// SolidWorks stores absolute Windows paths (e.g.
/// `C:\Engineering\Parts\Flange.SLDPRT`).  Supply one or more substitutions
/// to map these to paths that are reachable on the host filesystem.
struct PathSubstitution {
  /// Windows path prefix to match (case-insensitive, backslash or forward
  /// slash, trailing separator optional).
  /// Example: `"C:\\Engineering\\Parts\\"` or `"\\\\server\\share\\"`
  std::string windows_prefix;

  /// Host-filesystem prefix to replace it with (must end with `/`).
  /// Example: `"/mnt/nas/engineering/"`
  std::string local_prefix;
};

/// @brief Configuration for the Windows-to-host path resolver.
struct PathResolverConfig {
  /// Ordered list of prefix substitution rules.  Rules are tried in order;
  /// the first matching rule whose result exists on disk wins.
  std::vector<PathSubstitution> substitutions;

  /// Additional directories to search when no substitution rule resolves the
  /// path.  The resolver performs a shallow filename match (not recursive) in
  /// each listed directory, plus a recursive fallback if enabled.
  std::vector<std::filesystem::path> search_dirs;

  /// When `true`, filename comparisons against the host filesystem are done
  /// case-insensitively.  Useful when Windows paths use a different casing
  /// than the Linux NAS.  Default: `true`.
  bool case_insensitive = true;

  /// When `true`, the resolver also searches @ref search_dirs recursively
  /// (not just the top level) by filename.  May be slow on large trees.
  /// Default: `false`.
  bool recursive_search = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// BomTransformerConfig
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Full configuration for @ref BomTransformer.
struct BomTransformerConfig {
  // ── Configuration selection ─────────────────────────────────────────────

  /// Name of the SolidWorks configuration to activate.
  ///
  /// - Empty string (default): use the first configuration in the file.
  /// - Non-empty string: search by exact name; fall back to first config if
  ///   not found (a warning is emitted in that case).
  ///
  /// For drawing files the configuration is selected per model-view reference
  /// (the view stores the configuration name explicitly).
  std::string configuration_name;

  // ── Component filtering ─────────────────────────────────────────────────

  /// Include suppressed assembly components in the BOM.  Default: `false`.
  bool include_suppressed = false;

  /// Include hidden assembly components in the BOM.  Default: `false`.
  bool include_hidden = false;

  /// Include components that SolidWorks marks as "exclude from BOM".
  /// Default: `false`.
  bool include_exclude_from_bom = false;

  // ── Cut-list expansion ──────────────────────────────────────────────────

  /// When `true`, cut-list items of weldment parts are added as children of
  /// the part BomItem with @ref BomItemType::kCutListItem.  Default: `true`.
  bool expand_cut_lists = true;

  // ── Recursion ───────────────────────────────────────────────────────────

  /// Maximum recursion depth when expanding assembly components.
  ///
  /// - 0 (default): unlimited.
  /// - N > 0: stop expanding children at depth N (the root is depth 0).
  int max_depth = 0;

  // ── Path resolution ─────────────────────────────────────────────────────

  PathResolverConfig path_resolver;

  // ── Extension hook ──────────────────────────────────────────────────────

  /// Optional post-processing callback invoked for every BomItem immediately
  /// after it is fully constructed (including its children).
  ///
  /// Use this to inject additional properties, rename items, filter children,
  /// or compute derived fields without subclassing BomTransformer.
  ///
  /// The callback receives a mutable reference to the item.  Modifications
  /// to @ref BomItem::children at this point are preserved in the output.
  std::function<void(BomItem&)> item_callback;
};

// ─────────────────────────────────────────────────────────────────────────────
// BomTransformer
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Builds a hierarchical BOM from a SolidWorks file.
///
/// @ref Build() opens the root file, detects its type, selects the active
/// configuration, and recursively expands assembly components.  Identical
/// component instances (same resolved path + configuration) are aggregated
/// into a single @ref BomItem with @ref BomItem::quantity set to the instance
/// count.
///
/// **Thread safety:** Multiple threads may call @ref Build() on *different*
/// BomTransformer instances concurrently.  A single instance must not be used
/// from multiple threads simultaneously.
class BomTransformer {
 public:
  /// @brief Constructs a transformer with the given configuration.
  explicit BomTransformer(BomTransformerConfig config = {});

  /// @brief Builds the BOM for the file at @p path.
  ///
  /// The file may be a part (.SLDPRT), assembly (.SLDASM), or drawing
  /// (.SLDDRW).  Assembly components are resolved and opened recursively.
  ///
  /// @param path Filesystem path to the SolidWorks root document.
  /// @return `Result::Ok(bom)` on success.  Non-fatal issues (missing
  ///         components, unresolved paths, exceeded depth) are reported as
  ///         @ref Bom::warnings rather than errors.
  /// @return `Result::Err(message)` if the root file itself cannot be opened
  ///         or parsed (see @ref openswx::SwxDocument::Open()).
  [[nodiscard]] openswx::Result<Bom> Build(const std::filesystem::path& path);

 private:
  BomTransformerConfig config_;

  // ── Per-Build context (re-initialised on every Build() call) ────────────

  struct BuildContext {
    /// Parsed documents keyed by canonical path (avoids re-parsing the same
    /// file when it appears multiple times in the assembly tree).
    std::map<std::filesystem::path, openswx::Document> doc_cache;

    /// Paths currently on the call stack — used to detect reference cycles.
    std::set<std::filesystem::path> active_stack;

    /// Accumulated non-fatal warnings.
    std::vector<std::string> warnings;
  };

  // ── Internal builders ────────────────────────────────────────────────────

  BomItem BuildDrawing(BuildContext& ctx,
                       const std::filesystem::path& path,
                       const openswx::Document& doc);

  BomItem BuildAssembly(BuildContext& ctx,
                        const std::filesystem::path& path,
                        const openswx::Document& doc,
                        const std::string& config_override,
                        int depth);

  BomItem BuildPart(BuildContext& ctx,
                    const std::filesystem::path& path,
                    const openswx::Document& doc,
                    const std::string& config_override);

  BomItem BuildFromPath(BuildContext& ctx,
                        const std::filesystem::path& resolved_path,
                        const std::string& windows_path,
                        const std::string& instance_name,
                        const std::string& config_override,
                        int depth);

  // ── Helpers ──────────────────────────────────────────────────────────────

  const openswx::Configuration* SelectConfig(
      const openswx::Document& doc,
      const std::string& config_override,
      std::vector<std::string>& warnings) const;

  std::optional<std::filesystem::path> ResolvePath(
      const std::string& windows_path) const;

  const openswx::Document* GetOrParse(
      BuildContext& ctx,
      const std::filesystem::path& path,
      std::vector<std::string>& warnings) const;
};

}  // namespace openbom
