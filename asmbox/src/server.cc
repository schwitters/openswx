// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#include "crow.h"
#include <plog/Formatters/TxtFormatter.h>
#include <plog/Initializers/ConsoleInitializer.h>
#include <plog/Log.h>
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include "openbom/json_writer.h"
#include "openbom/transformer.h"
#include "storage/sqlite_storage.h"
#include "sw/document_analyzer.h"
#include "utils/string_utils.h"
#include "utils/zip_extractor.h"

namespace fs = std::filesystem;
namespace sw = sw_dumper::sw;
namespace utils = sw_dumper::utils;

// ── Runtime configuration ─────────────────────────────────────────────────────
//
// Every setting can be overridden via an environment variable:
//
//   ASMBOX_BIND_ADDR    Bind address              (default: 0.0.0.0)
//   ASMBOX_PORT         TCP port                  (default: 8087)
//   ASMBOX_DATA_DIR     Workspace / DB root       (default: $TMPDIR/sw_portal)
//   ASMBOX_TEMPLATE_DIR Crow mustache template dir (default: templates)

struct Config {
  std::string  bind_addr;
  std::uint16_t port;
  fs::path     data_dir;
  std::string  template_dir;
};

static std::string EnvOr(const char* name, std::string default_value) {
  const char* v = std::getenv(name);
  return (v && *v) ? v : default_value;
}

static Config LoadConfig() {
  Config cfg;
  cfg.bind_addr    = EnvOr("ASMBOX_BIND_ADDR", "0.0.0.0");
  cfg.template_dir = EnvOr("ASMBOX_TEMPLATE_DIR", "templates");
  cfg.data_dir     = EnvOr("ASMBOX_DATA_DIR",
                            (fs::temp_directory_path() / "sw_portal").string());

  // Parse port — fall back to default on invalid input.
  std::string port_str = EnvOr("ASMBOX_PORT", "8087");
  std::uint16_t port = 8087;
  auto [ptr, ec] = std::from_chars(port_str.data(),
                                    port_str.data() + port_str.size(), port);
  if (ec != std::errc{}) {
    PLOG_WARNING << "ASMBOX_PORT='" << port_str << "' is invalid, using 8087";
    port = 8087;
  }
  cfg.port = port;

  return cfg;
}

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string SanitizeFilename(std::string name) {
  for (char& c : name)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '.')
      c = '_';
  return name;
}

// ── BOM transformation ────────────────────────────────────────────────────────
//
// Apply profile rules to a BOM item node (recursive, depth-first).
// Each rule: { rule_type, property_name, property_value }
//
//   non_bom    — item is excluded from BOM entirely
//   phantom    — item itself is excluded; its children are promoted to parent
//   kaufgruppe — item is kept but all its children are removed
//
static nlohmann::json ApplyBomRules(const nlohmann::json& item,
                                    const nlohmann::json& rules) {
  nlohmann::json result = item;
  if (!result.contains("children") || !result["children"].is_array())
    return result;

  nlohmann::json new_children = nlohmann::json::array();
  for (const auto& child : result["children"]) {
    // Recurse first (bottom-up) so promoted grandchildren also get transformed.
    nlohmann::json tc = ApplyBomRules(child, rules);

    bool non_bom = false, phantom = false, kaufgruppe = false;
    const nlohmann::json& props =
        (tc.contains("properties") && tc["properties"].is_object())
            ? tc["properties"]
            : nlohmann::json::object();

    for (const auto& rule : rules) {
      if (!rule.is_object()) continue;
      std::string rt = rule.value("rule_type",      "");
      std::string pn = rule.value("property_name",  "");
      std::string pv = rule.value("property_value", "");
      if (pn.empty()) continue;
      if (props.contains(pn) && props[pn].is_string() &&
          props[pn].get<std::string>() == pv) {
        if (rt == "non_bom")    non_bom    = true;
        if (rt == "phantom")    phantom    = true;
        if (rt == "kaufgruppe") kaufgruppe = true;
      }
    }

    if (non_bom) {
      // Drop entirely.
    } else if (phantom) {
      // Replace with children.
      const auto& grandchildren = tc.contains("children") && tc["children"].is_array()
                                      ? tc["children"]
                                      : nlohmann::json::array();
      for (const auto& gc : grandchildren) new_children.push_back(gc);
    } else if (kaufgruppe) {
      tc["children"] = nlohmann::json::array();
      new_children.push_back(tc);
    } else {
      new_children.push_back(tc);
    }
  }

  result["children"] = new_children;
  return result;
}

static void DeleteDirectory(const fs::path& path) {
  try {
    if (fs::exists(path)) fs::remove_all(path);
  } catch (...) {}
}

// ── main ─────────────────────────────────────────────────────────────────────

int main() {
  static plog::ConsoleAppender<plog::TxtFormatter> console_appender;
  plog::init(plog::debug, &console_appender);

  const Config cfg = LoadConfig();
  PLOG_INFO << "bind_addr    = " << cfg.bind_addr;
  PLOG_INFO << "port         = " << cfg.port;
  PLOG_INFO << "data_dir     = " << cfg.data_dir.string();
  PLOG_INFO << "template_dir = " << cfg.template_dir;

  // Crow mustache template loader — set_base() only updates a static that is
  // read before worker threads start, so we use set_loader() which is called
  // at render time and therefore always sees the captured path.
  crow::mustache::set_loader(
      [template_dir = cfg.template_dir](const std::string& filename) -> std::string {
        fs::path full_path = fs::path(template_dir) / filename;
        std::ifstream f(full_path);
        if (!f) {
          PLOG_WARNING << "Template not found: " << full_path.string();
          return {};
        }
        return {std::istreambuf_iterator<char>(f),
                std::istreambuf_iterator<char>{}};
      });

  // Database — persistent SQLite file; survives server restarts.
  // Schema uses CREATE TABLE IF NOT EXISTS so it is safe to reuse an existing DB.
  fs::path global_db_path = cfg.data_dir / "global.sqlite";
  auto storage = std::make_shared<sw_dumper::storage::SqliteStorage>(
      global_db_path.string());
  PLOG_INFO << "DB initialized: " << global_db_path.string();

  crow::SimpleApp app;

  CROW_ROUTE(app, "/")([]() {
    return crow::mustache::load_text("index.html");
  });

  // ── GET /api/workspaces ────────────────────────────────────────────────────
  CROW_ROUTE(app, "/api/workspaces")
      .methods(crow::HTTPMethod::Get)([storage]() {
        auto ws = storage->GetWorkspaces();
        nlohmann::json j = nlohmann::json::array();
        for (const auto& w : ws)
          j.push_back({{"name", w.name}, {"created_at", w.created_at}});
        return crow::response(j.dump());
      });

  // ── DELETE /api/workspaces/<name> ─────────────────────────────────────────
  CROW_ROUTE(app, "/api/workspaces/<string>")
      .methods(crow::HTTPMethod::Delete)(
          [storage, data_dir = cfg.data_dir](std::string name) {
            if (!storage->DeleteWorkspace(name))
              return crow::response(404);
            DeleteDirectory(data_dir / name);
            return crow::response(200);
          });

  // ── POST /upload ──────────────────────────────────────────────────────────
  CROW_ROUTE(app, "/upload")
      .methods(crow::HTTPMethod::Post)(
          [storage, data_dir = cfg.data_dir](const crow::request& req) {
            try {
              crow::multipart::message msg(req);
              auto zip_part = msg.get_part_by_name("file");
              if (zip_part.body.empty())
                return crow::response(400);

              std::string filename = "Upload";
              auto disp =
                  zip_part.get_header_object("Content-Disposition").params;
              if (disp.find("filename") != disp.end())
                filename = disp.at("filename");

              auto dot = filename.find_last_of('.');
              if (dot != std::string::npos) filename = filename.substr(0, dot);
              std::string workspace = SanitizeFilename(filename);
              if (workspace.empty()) workspace = "Default";

              PLOG_INFO << "Creating workspace: " << workspace;
              fs::path sandbox = data_dir / workspace;
              DeleteDirectory(sandbox);

              const utils::ZipExtractionResult unzip_result =
                  utils::UnzipToFolder(zip_part.body, sandbox);
              if (!unzip_result.ok()) {
                PLOG_WARNING << "Rejected ZIP upload: " << unzip_result.error();
                DeleteDirectory(sandbox);
                return crow::response(400, unzip_result.error());
              }

              storage->DeleteWorkspace(workspace);
              if (!storage->CreateWorkspace(workspace))
                return crow::response(500, "DB error");

              // Enumerate SolidWorks files and eagerly analyze each one so the
              // property index is populated immediately after upload.
              nlohmann::json file_list = nlohmann::json::array();
              for (const auto& entry :
                   fs::recursive_directory_iterator(sandbox)) {
                if (!entry.is_regular_file()) continue;
                std::string ext = entry.path().extension().string();
                if (!utils::EndsWithCI(ext, ".sldprt") &&
                    !utils::EndsWithCI(ext, ".sldasm") &&
                    !utils::EndsWithCI(ext, ".slddrw"))
                  continue;

                std::string rel = fs::relative(entry.path(), sandbox).string();
                file_list.push_back({
                    {"name",     entry.path().filename().string()},
                    {"rel_path", rel},
                    {"session",  workspace},
                });

                // Analyze and cache in SQLite if not already there.
                nlohmann::json probe;
                if (!storage->Load(workspace, rel, &probe)) {
                  nlohmann::json doc_data;
                  sw::DocumentAnalyzer analyzer;
                  PLOG_INFO << "Scanning: " << rel;
                  if (analyzer.AnalyzeFile(entry.path(), &doc_data))
                    storage->Save(workspace, rel, doc_data);
                  else
                    PLOG_WARNING << "Scan failed: " << rel;
                }
              }
              return crow::response(file_list.dump());
            } catch (const std::exception& e) {
              PLOG_FATAL << e.what();
              return crow::response(500);
            }
          });

  // ── GET /api/doc/<workspace>/<path_base64> ────────────────────────────────
  // Returns the parsed document as raw JSON (same data as stored in SQLite).
  // Used by the frontend for inline rendering without an iframe.
  CROW_ROUTE(app, "/api/doc/<string>/<string>")
      ([storage, data_dir = cfg.data_dir](std::string workspace,
                                           std::string path_b64) {
        try {
          std::string decoded_path = utils::Base64UrlDecode(path_b64);
          if (decoded_path.find("..") != std::string::npos)
            return crow::response(400, "Invalid path");

          fs::path f_path = data_dir / workspace / decoded_path;

          nlohmann::json data;
          if (storage->Load(workspace, decoded_path, &data)) {
            PLOG_INFO << "API doc cache hit: " << decoded_path;
          } else {
            if (!fs::exists(f_path))
              return crow::response(404, "File not found");
            PLOG_INFO << "API doc analyzing: " << decoded_path;
            sw::DocumentAnalyzer analyzer;
            if (!analyzer.AnalyzeFile(f_path, &data))
              return crow::response(500, "Analysis failed");
            storage->Save(workspace, decoded_path, data);
          }

          crow::response res(data.dump());
          res.set_header("Content-Type", "application/json; charset=utf-8");
          return res;
        } catch (const std::exception& e) {
          PLOG_FATAL << e.what();
          return crow::response(500);
        }
      });

  // ── GET|POST /api/workspaces/<ws>/props ──────────────────────────────────
  // GET  → { names:[...], configs:[{name,visible,role},...] }
  // POST → body = [{name,visible,role},...] — saves config, returns 200
  CROW_ROUTE(app, "/api/workspaces/<string>/props")
      .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Post)(
          [storage](const crow::request& req, std::string ws) {
            if (req.method == crow::HTTPMethod::Post) {
              try {
                auto body = nlohmann::json::parse(req.body);
                if (!storage->SetPropertyConfig(ws, body))
                  return crow::response(500, "DB error");
                return crow::response(200);
              } catch (...) {
                return crow::response(400, "Invalid JSON");
              }
            }

            // GET
            auto names   = storage->GetPropertyNames(ws);
            auto configs = storage->GetPropertyConfig(ws);

            std::map<std::string, nlohmann::json> cfg_map;
            for (const auto& c : configs) cfg_map[c["name"].get<std::string>()] = c;

            nlohmann::json merged = nlohmann::json::array();
            for (const auto& n : names) {
              merged.push_back(cfg_map.count(n)
                ? cfg_map.at(n)
                : nlohmann::json{{"name", n}, {"visible", true}, {"role", ""}});
            }
            for (const auto& [n, c] : cfg_map)
              if (std::find(names.begin(), names.end(), n) == names.end())
                merged.push_back(c);

            nlohmann::json resp;
            resp["names"]   = names;
            resp["configs"] = merged;
            crow::response res(resp.dump());
            res.set_header("Content-Type", "application/json; charset=utf-8");
            return res;
          });

  // ── Profile CRUD (/api/profiles[/<id>]) ──────────────────────────────────
  // GET  /api/profiles           → list all profiles
  // POST /api/profiles           → create {name, description}
  // GET  /api/profiles/<id>      → full profile (mappings + rules)
  // PUT  /api/profiles/<id>      → save full profile
  // DELETE /api/profiles/<id>    → delete
  CROW_ROUTE(app, "/api/profiles")
      .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Post)(
          [storage](const crow::request& req) {
            if (req.method == crow::HTTPMethod::Post) {
              try {
                auto body = nlohmann::json::parse(req.body);
                std::string name = body.value("name", "");
                std::string desc = body.value("description", "");
                if (name.empty()) return crow::response(400, "name required");
                int64_t id = storage->CreateProfile(name, desc);
                if (id < 0) return crow::response(409, "Name already exists");
                nlohmann::json j; j["id"] = id;
                return crow::response(j.dump());
              } catch (...) { return crow::response(400, "Invalid JSON"); }
            }
            // GET
            crow::response res(storage->GetProfiles().dump());
            res.set_header("Content-Type", "application/json; charset=utf-8");
            return res;
          });

  CROW_ROUTE(app, "/api/profiles/<int>")
      .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Put,
               crow::HTTPMethod::Delete)(
          [storage](const crow::request& req, int profile_id) {
            if (req.method == crow::HTTPMethod::Delete) {
              return storage->DeleteProfile(profile_id)
                  ? crow::response(200) : crow::response(404);
            }
            if (req.method == crow::HTTPMethod::Put) {
              try {
                auto body = nlohmann::json::parse(req.body);
                body["id"] = profile_id;
                return storage->SaveProfile(body)
                    ? crow::response(200) : crow::response(400, "Save failed");
              } catch (...) { return crow::response(400, "Invalid JSON"); }
            }
            // GET
            auto p = storage->GetProfile(profile_id);
            if (p.is_null() || p.empty()) return crow::response(404);
            crow::response res(p.dump());
            res.set_header("Content-Type", "application/json; charset=utf-8");
            return res;
          });

  // ── GET /api/bom/<workspace>/<path_base64> ────────────────────────────────
  // Builds a recursive BOM using libopenbom and persists it to SQLite.
  CROW_ROUTE(app, "/api/bom/<string>/<string>")
      ([storage, data_dir = cfg.data_dir](const crow::request& req,
                                           std::string workspace,
                                           std::string path_b64) {
        try {
          std::string decoded_path = utils::Base64UrlDecode(path_b64);
          if (decoded_path.find("..") != std::string::npos)
            return crow::response(400, "Invalid path");

          fs::path f_path = data_dir / workspace / decoded_path;
          if (!fs::exists(f_path))
            return crow::response(404, "File not found");

          PLOG_INFO << "Building BOM: " << decoded_path;

          openbom::BomTransformerConfig bom_cfg;
          bom_cfg.path_resolver.search_dirs.push_back(data_dir / workspace);
          bom_cfg.path_resolver.recursive_search = true;
          bom_cfg.path_resolver.case_insensitive = true;

          openbom::BomTransformer transformer(bom_cfg);
          auto result = transformer.Build(f_path);
          if (!result.ok())
            return crow::response(500, result.error());

          openbom::JsonWriter writer(false);
          std::string json_str = writer.Write(result.value());

          // Optionally apply profile transformation rules.
          nlohmann::json bom_json;
          try {
            bom_json = nlohmann::json::parse(json_str);
          } catch (...) {}

          // Check for ?profile=<id> query parameter.
          std::string profile_param = req.url_params.get("profile") ?
              req.url_params.get("profile") : "";
          if (!profile_param.empty() && bom_json.contains("bom")) {
            try {
              int64_t pid = std::stoll(profile_param);
              auto profile = storage->GetProfile(pid);
              if (!profile.empty() && profile.contains("rules") &&
                  profile["rules"].is_array() && !profile["rules"].empty()) {
                bom_json["bom"] = ApplyBomRules(bom_json["bom"], profile["rules"]);
                json_str = bom_json.dump();
                PLOG_INFO << "Applied profile " << pid << " rules to BOM";
              }
            } catch (...) {}
          }

          // Persist BOM to SQLite using surrogate keys.
          try {
            if (bom_json.contains("bom")) {
              std::string pn_prop = storage->GetPartNumberProp(workspace);
              std::string cfg_name = bom_json["bom"].value("configuration", "");
              storage->SaveBom(workspace, decoded_path, cfg_name, pn_prop,
                               bom_json["bom"]);
            }
          } catch (const std::exception& pe) {
            PLOG_WARNING << "BOM persist failed: " << pe.what();
          }

          crow::response res(json_str);
          res.set_header("Content-Type", "application/json; charset=utf-8");
          return res;
        } catch (const std::exception& e) {
          PLOG_FATAL << e.what();
          return crow::response(500);
        }
      });

  app.bindaddr(cfg.bind_addr).port(cfg.port).multithreaded().run();
  return EXIT_SUCCESS;
}
