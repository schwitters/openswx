// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#include "path_resolver.h"

#include <algorithm>
#include <cctype>

namespace openbom::internal {

PathResolver::PathResolver(const PathResolverConfig& cfg) : cfg_(cfg) {}

// ── Static helpers ──────────────────────────────────────────────────────────

std::string PathResolver::NormalizeSeparators(const std::string& p) {
  std::string out = p;
  std::replace(out.begin(), out.end(), '\\', '/');
  return out;
}

std::optional<std::filesystem::path> PathResolver::TryExact(
    const std::filesystem::path& p) {
  std::error_code ec;
  if (std::filesystem::exists(p, ec) && !ec) return p;
  return std::nullopt;
}

std::optional<std::filesystem::path> PathResolver::TryCaseInsensitive(
    const std::filesystem::path& candidate) {
  // Start from the root (e.g. "/" on Linux) and walk each component.
  std::filesystem::path result = candidate.root_path();
  if (result.empty()) return std::nullopt;

  for (const auto& comp : candidate.relative_path()) {
    std::string comp_str  = comp.string();
    std::string comp_lower = comp_str;
    std::transform(comp_lower.begin(), comp_lower.end(),
                   comp_lower.begin(), [](unsigned char c) {
                     return static_cast<char>(std::tolower(c));
                   });

    bool found = false;
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::directory_iterator(result, ec)) {
      if (ec) break;
      std::string entry_name  = entry.path().filename().string();
      std::string entry_lower = entry_name;
      std::transform(entry_lower.begin(), entry_lower.end(),
                     entry_lower.begin(), [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                     });
      if (entry_lower == comp_lower) {
        result /= entry.path().filename();
        found = true;
        break;
      }
    }
    if (!found) return std::nullopt;
  }
  return result;
}

// ── Search by filename ───────────────────────────────────────────────────────

std::optional<std::filesystem::path> PathResolver::SearchByFilename(
    const std::filesystem::path& p) const {
  std::string filename = p.filename().string();
  std::string filename_lower = filename;
  std::transform(filename_lower.begin(), filename_lower.end(),
                 filename_lower.begin(), [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });

  for (const auto& search_dir : cfg_.search_dirs) {
    std::error_code ec;
    auto try_match = [&](const std::filesystem::path& ep)
        -> std::optional<std::filesystem::path> {
      std::string en = ep.filename().string();
      if (cfg_.case_insensitive) {
        std::string en_lower = en;
        std::transform(en_lower.begin(), en_lower.end(), en_lower.begin(),
                       [](unsigned char c) {
                         return static_cast<char>(std::tolower(c));
                       });
        if (en_lower == filename_lower) return ep;
      } else {
        if (en == filename) return ep;
      }
      return std::nullopt;
    };

    // Shallow search
    for (const auto& entry :
         std::filesystem::directory_iterator(search_dir, ec)) {
      if (ec) break;
      if (auto r = try_match(entry.path())) return r;
    }

    // Optionally recursive
    if (cfg_.recursive_search) {
      ec.clear();
      for (const auto& entry :
           std::filesystem::recursive_directory_iterator(search_dir, ec)) {
        if (ec) break;
        if (auto r = try_match(entry.path())) return r;
      }
    }
  }
  return std::nullopt;
}

// ── Public Resolve ───────────────────────────────────────────────────────────

std::optional<std::filesystem::path> PathResolver::Resolve(
    const std::string& windows_path) const {
  if (windows_path.empty()) return std::nullopt;

  std::string norm = NormalizeSeparators(windows_path);

  // Build a lowercase version of norm for prefix matching.
  std::string norm_lower = norm;
  std::transform(norm_lower.begin(), norm_lower.end(), norm_lower.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });

  for (const auto& sub : cfg_.substitutions) {
    std::string prefix_norm  = NormalizeSeparators(sub.windows_prefix);
    std::string prefix_lower = prefix_norm;
    std::transform(prefix_lower.begin(), prefix_lower.end(),
                   prefix_lower.begin(), [](unsigned char c) {
                     return static_cast<char>(std::tolower(c));
                   });

    // Ensure the prefix ends with '/' for clean suffix extraction.
    if (!prefix_lower.empty() && prefix_lower.back() != '/')
      prefix_lower += '/';
    if (!prefix_norm.empty() && prefix_norm.back() != '/')
      prefix_norm += '/';

    if (norm_lower.size() < prefix_lower.size()) continue;
    if (norm_lower.substr(0, prefix_lower.size()) != prefix_lower) continue;

    // Found a matching prefix — build candidate path.
    std::string suffix = norm.substr(prefix_norm.size());
    std::filesystem::path local_base = sub.local_prefix;
    // Strip trailing slash from local_prefix to avoid double slashes.
    if (!local_base.empty() &&
        local_base.string().back() == '/') {
      local_base = std::filesystem::path(
          local_base.string().substr(0, local_base.string().size() - 1));
    }
    std::filesystem::path candidate = local_base / std::filesystem::path(suffix);

    if (auto r = TryExact(candidate)) return r;
    if (cfg_.case_insensitive) {
      if (auto r = TryCaseInsensitive(candidate)) return r;
    }
  }

  // No substitution resolved — fall back to filename search.
  return SearchByFilename(std::filesystem::path(norm));
}

}  // namespace openbom::internal
