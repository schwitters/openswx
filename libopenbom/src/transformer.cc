// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#include "openbom/transformer.h"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <map>
#include <set>
#include <string>

#include "openswx/document.h"
#include "path_resolver.h"

namespace openbom {

namespace {

// ── Name utilities ───────────────────────────────────────────────────────────

// Strips the SolidWorks instance suffix "-N" (e.g. "Bolt-3" → "Bolt").
// Returns the original name if the suffix does not match the pattern.
std::string StripInstanceSuffix(const std::string& name) {
  if (name.empty()) return name;
  auto dash = name.rfind('-');
  if (dash == std::string::npos || dash == 0) return name;
  auto suffix = name.substr(dash + 1);
  if (suffix.empty()) return name;
  bool all_digits = std::all_of(suffix.begin(), suffix.end(),
                                [](unsigned char c) { return std::isdigit(c) != 0; });
  return all_digits ? name.substr(0, dash) : name;
}

// Derives a display name from a filesystem path stem.
std::string StemName(const std::filesystem::path& p) {
  return p.stem().string();
}

// Merges global_properties (base) with cfg properties (override).
std::map<std::string, std::string> MergeProperties(
    const std::map<std::string, std::string>& global_props,
    const std::map<std::string, std::string>& cfg_props) {
  auto merged = global_props;
  for (const auto& [k, v] : cfg_props) merged.insert_or_assign(k, v);
  return merged;
}

// ── Aggregation key ───────────────────────────────────────────────────────────

// Key used to group identical assembly component instances together.
// Primary key: resolved host path (when available) or Windows path, plus
// the active configuration name.
struct AggKey {
  std::string path_key;   // resolved path or windows_path
  std::string config;

  bool operator<(const AggKey& o) const {
    if (path_key != o.path_key) return path_key < o.path_key;
    return config < o.config;
  }
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// BomTransformer — construction
// ─────────────────────────────────────────────────────────────────────────────

BomTransformer::BomTransformer(BomTransformerConfig config)
    : config_(std::move(config)) {}

// ─────────────────────────────────────────────────────────────────────────────
// BomTransformer — public Build()
// ─────────────────────────────────────────────────────────────────────────────

openswx::Result<Bom> BomTransformer::Build(const std::filesystem::path& path) {
  // Open and parse the root document.
  auto open_result = openswx::SwxDocument::Open(path);
  if (!open_result.ok()) return openswx::Result<Bom>::Err(open_result.error());

  BuildContext ctx;

  // Store root document in cache (canonical path → avoids re-open on cycles).
  std::error_code ec;
  std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
  if (ec) canonical = path;

  // Move the document into the cache, then reference the cached copy.
  // Important: take the reference AFTER emplacing — not before — so that
  // doc_ref never points at a moved-from object.
  auto emplace_result =
      ctx.doc_cache.emplace(canonical, std::move(open_result).value().doc());
  const openswx::Document& doc_ref = emplace_result.first->second;
  ctx.active_stack.insert(canonical);

  BomItem root;
  switch (doc_ref.type) {
    case openswx::DocumentType::kDrawing:
      root = BuildDrawing(ctx, canonical, doc_ref);
      break;
    case openswx::DocumentType::kAssembly:
      root = BuildAssembly(ctx, canonical, doc_ref,
                           config_.configuration_name, 0);
      break;
    case openswx::DocumentType::kPart:
      root = BuildPart(ctx, canonical, doc_ref, config_.configuration_name);
      break;
  }

  ctx.active_stack.erase(canonical);

  // Fire the item callback on the root (children already had it fired).
  if (config_.item_callback) config_.item_callback(root);

  Bom bom;
  bom.root     = std::move(root);
  bom.warnings = std::move(ctx.warnings);
  return openswx::Result<Bom>::Ok(std::move(bom));
}

// ─────────────────────────────────────────────────────────────────────────────
// BuildDrawing
// ─────────────────────────────────────────────────────────────────────────────

BomItem BomTransformer::BuildDrawing(BuildContext& ctx,
                                     const std::filesystem::path& path,
                                     const openswx::Document& doc) {
  BomItem item;
  item.type      = BomItemType::kDrawing;
  item.name      = StemName(path);
  item.file_path = path.string();
  item.quantity  = 1;

  // Collect unique (windows_path, configuration) pairs across all sheets.
  // Preserve insertion order via an ordered set + insertion-order vector.
  struct ViewRef {
    std::string windows_path;
    std::string configuration;
    bool operator<(const ViewRef& o) const {
      if (windows_path != o.windows_path) return windows_path < o.windows_path;
      return configuration < o.configuration;
    }
  };
  std::set<ViewRef>        seen;
  std::vector<ViewRef>     ordered;

  for (const auto& sheet : doc.sheets) {
    for (const auto& view : sheet.views) {
      if (view.referenced_document.empty()) continue;
      ViewRef vr{view.referenced_document, view.referenced_configuration};
      if (seen.insert(vr).second) ordered.push_back(vr);
    }
  }

  for (const auto& vr : ordered) {
    auto resolved = ResolvePath(vr.windows_path);
    if (!resolved) {
      ctx.warnings.push_back(
          "Drawing model not resolved: " + vr.windows_path);
      // Still add a placeholder so the drawing structure is visible.
      BomItem placeholder;
      placeholder.type         = BomItemType::kPart;
      placeholder.name         = std::filesystem::path(vr.windows_path).stem().string();
      placeholder.windows_path = vr.windows_path;
      placeholder.configuration = vr.configuration;
      placeholder.quantity     = 1;
      if (config_.item_callback) config_.item_callback(placeholder);
      item.children.push_back(std::move(placeholder));
      continue;
    }

    BomItem child = BuildFromPath(ctx, *resolved, vr.windows_path,
                                  StemName(*resolved), vr.configuration, 1);
    item.children.push_back(std::move(child));
  }

  return item;
}

// ─────────────────────────────────────────────────────────────────────────────
// BuildAssembly
// ─────────────────────────────────────────────────────────────────────────────

BomItem BomTransformer::BuildAssembly(BuildContext& ctx,
                                      const std::filesystem::path& path,
                                      const openswx::Document& doc,
                                      const std::string& config_override,
                                      int depth) {
  BomItem item;
  item.type      = BomItemType::kAssembly;
  item.name      = StemName(path);
  item.file_path = path.string();
  item.quantity  = 1;

  const openswx::Configuration* cfg =
      SelectConfig(doc, config_override, ctx.warnings);

  if (cfg) {
    item.configuration = cfg->name;
    item.properties    = MergeProperties(doc.global_properties, cfg->properties);
  } else {
    item.properties = doc.global_properties;
  }

  // ── Aggregate components ────────────────────────────────────────────────

  if (!cfg || cfg->components.empty()) return item;

  // Check depth limit.
  if (config_.max_depth > 0 && depth >= config_.max_depth) {
    ctx.warnings.push_back(
        "Max recursion depth reached; children omitted for: " + path.string());
    return item;
  }

  // Group component instances by (path_key, config_name).
  // Preserve first-occurrence order so the BOM matches SolidWorks ordering.
  struct Group {
    const openswx::Component* first_comp;  // prototype instance
    std::string resolved_path_str;         // "" if not resolved
    int count = 0;
  };

  std::map<AggKey, Group> groups;
  std::vector<AggKey>     order;  // insertion order

  internal::PathResolver resolver(config_.path_resolver);

  for (const auto& comp : cfg->components) {
    // Apply visibility filters.
    if (!config_.include_suppressed && comp.is_suppressed) continue;
    if (!config_.include_hidden     && comp.is_hidden)     continue;
    if (!config_.include_exclude_from_bom && comp.exclude_from_bom) continue;

    // Resolve path.
    std::string resolved_str;
    if (!comp.path.empty()) {
      auto r = resolver.Resolve(comp.path);
      if (r) resolved_str = r->string();
    }

    std::string path_key =
        resolved_str.empty() ? comp.path : resolved_str;

    // When both path and name-prefix fail to distinguish instances,
    // fall back to the stripped instance name.
    if (path_key.empty()) path_key = StripInstanceSuffix(comp.name);

    AggKey key{path_key, comp.configuration_name};

    auto it = groups.find(key);
    if (it == groups.end()) {
      groups[key] = {&comp, resolved_str, 1};
      order.push_back(key);
    } else {
      ++it->second.count;
    }
  }

  // ── Build child BomItems ─────────────────────────────────────────────────

  for (const auto& key : order) {
    const auto& grp  = groups.at(key);
    const auto& comp = *grp.first_comp;
    int qty = grp.count;

    BomItem child;

    if (!grp.resolved_path_str.empty()) {
      std::filesystem::path child_path(grp.resolved_path_str);
      child = BuildFromPath(ctx, child_path, comp.path,
                            StripInstanceSuffix(comp.name),
                            comp.configuration_name, depth + 1);
    } else {
      // File not found — build a minimal placeholder.
      if (!comp.path.empty()) {
        ctx.warnings.push_back("Component not resolved: " + comp.path);
      }
      child.type             = BomItemType::kPart;
      child.name             = StripInstanceSuffix(comp.name);
      child.windows_path     = comp.path;
      child.configuration    = comp.configuration_name;
      child.is_suppressed    = comp.is_suppressed;
      child.exclude_from_bom = comp.exclude_from_bom;
    }

    child.quantity = qty;

    if (config_.item_callback) config_.item_callback(child);
    item.children.push_back(std::move(child));
  }

  return item;
}

// ─────────────────────────────────────────────────────────────────────────────
// BuildPart
// ─────────────────────────────────────────────────────────────────────────────

BomItem BomTransformer::BuildPart(BuildContext& ctx,
                                  const std::filesystem::path& path,
                                  const openswx::Document& doc,
                                  const std::string& config_override) {
  BomItem item;
  item.type      = BomItemType::kPart;
  item.name      = StemName(path);
  item.file_path = path.string();
  item.quantity  = 1;

  const openswx::Configuration* cfg =
      SelectConfig(doc, config_override, ctx.warnings);

  if (cfg) {
    item.configuration = cfg->name;
    item.properties    = MergeProperties(doc.global_properties, cfg->properties);

    // ── Expand cut list ───────────────────────────────────────────────────
    if (config_.expand_cut_lists && !cfg->cut_list.empty()) {
      for (const auto& cli : cfg->cut_list) {
        BomItem cut;
        cut.type          = BomItemType::kCutListItem;
        cut.name          = cli.name;
        cut.configuration = item.configuration;
        cut.quantity      = cli.quantity > 0 ? cli.quantity : 1;
        cut.properties    = cli.properties;
        // Cut-list items inherit the part's resolved path for traceability.
        cut.file_path     = item.file_path;

        if (config_.item_callback) config_.item_callback(cut);
        item.children.push_back(std::move(cut));
      }
    }
  } else {
    item.properties = doc.global_properties;
  }

  return item;
}

// ─────────────────────────────────────────────────────────────────────────────
// BuildFromPath — dispatcher for already-resolved child paths
// ─────────────────────────────────────────────────────────────────────────────

BomItem BomTransformer::BuildFromPath(BuildContext& ctx,
                                      const std::filesystem::path& resolved_path,
                                      const std::string& windows_path,
                                      const std::string& instance_name,
                                      const std::string& config_override,
                                      int depth) {
  // Canonicalise for cache lookup / cycle detection.
  std::error_code ec;
  std::filesystem::path canon =
      std::filesystem::weakly_canonical(resolved_path, ec);
  if (ec) canon = resolved_path;

  // Cycle detection.
  if (ctx.active_stack.count(canon)) {
    ctx.warnings.push_back("Reference cycle detected; skipping: " +
                           canon.string());
    BomItem placeholder;
    placeholder.type         = BomItemType::kPart;
    placeholder.name         = instance_name;
    placeholder.file_path    = canon.string();
    placeholder.windows_path = windows_path;
    return placeholder;
  }

  const openswx::Document* doc = GetOrParse(ctx, canon, ctx.warnings);
  if (!doc) {
    BomItem placeholder;
    placeholder.type         = BomItemType::kPart;
    placeholder.name         = instance_name;
    placeholder.file_path    = canon.string();
    placeholder.windows_path = windows_path;
    return placeholder;
  }

  ctx.active_stack.insert(canon);

  BomItem item;
  switch (doc->type) {
    case openswx::DocumentType::kAssembly:
      item = BuildAssembly(ctx, canon, *doc, config_override, depth);
      break;
    case openswx::DocumentType::kPart:
      item = BuildPart(ctx, canon, *doc, config_override);
      break;
    case openswx::DocumentType::kDrawing:
      // Sub-drawings are unusual but handle gracefully.
      item = BuildDrawing(ctx, canon, *doc);
      break;
  }

  ctx.active_stack.erase(canon);

  // Override name with the instance name from the parent assembly so that the
  // BOM shows the assembly-level label rather than the file stem.
  if (!instance_name.empty()) item.name = instance_name;
  item.windows_path = windows_path;

  return item;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

const openswx::Configuration* BomTransformer::SelectConfig(
    const openswx::Document& doc,
    const std::string& config_override,
    std::vector<std::string>& warnings) const {
  if (doc.configurations.empty()) return nullptr;

  const std::string& target =
      config_override.empty() ? config_.configuration_name : config_override;

  if (!target.empty()) {
    for (const auto& cfg : doc.configurations) {
      if (cfg.name == target) return &cfg;
    }
    warnings.push_back("Configuration \"" + target +
                       "\" not found; using first configuration.");
  }
  return &doc.configurations.front();
}

std::optional<std::filesystem::path> BomTransformer::ResolvePath(
    const std::string& windows_path) const {
  internal::PathResolver resolver(config_.path_resolver);
  return resolver.Resolve(windows_path);
}

const openswx::Document* BomTransformer::GetOrParse(
    BuildContext& ctx,
    const std::filesystem::path& path,
    std::vector<std::string>& warnings) const {
  auto it = ctx.doc_cache.find(path);
  if (it != ctx.doc_cache.end()) return &it->second;

  auto result = openswx::SwxDocument::Open(path);
  if (!result.ok()) {
    warnings.push_back("Failed to open \"" + path.string() +
                       "\": " + result.error());
    return nullptr;
  }
  auto [emplace_it, ok] =
      ctx.doc_cache.emplace(path, std::move(result).value().doc());
  (void)ok;
  return &emplace_it->second;
}

}  // namespace openbom
