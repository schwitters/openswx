// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#include "crow.h"
#include <chrono>
#include <plog/Formatters/TxtFormatter.h>
#include <plog/Initializers/ConsoleInitializer.h>
#include <plog/Log.h>
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <unordered_map>
#include "auth/auth_utils.h"
#include "openbom/json_writer.h"
#include "openbom/transformer.h"
#include "storage/metadata_storage.h"
#include "storage/sqlite_storage.h"
#include "sw/document_analyzer.h"
#include "utils/string_utils.h"
#include "utils/zip_extractor.h"

namespace fs = std::filesystem;
namespace auth = sw_dumper::auth;
namespace storage = sw_dumper::storage;
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
//   ASMBOX_ADMIN_USER   Optional bootstrap admin username
//   ASMBOX_ADMIN_PASSWORD Optional bootstrap admin password

struct Config {
  std::string  bind_addr;
  std::uint16_t port;
  fs::path     data_dir;
  std::string  template_dir;
};

struct UserContext {
  int64_t user_id = -1;
  std::string username;
  bool is_admin = false;
  std::string session_token;
};

constexpr char kSessionCookieName[] = "asmbox_session";
constexpr int64_t kSessionLifetimeSeconds = 60 * 60 * 24 * 30;

static std::string EnvOr(const char* name, std::string default_value) {
  const char* v = std::getenv(name);
  return (v && *v) ? v : default_value;
}

static fs::path DefaultTemplateDir() {
  std::error_code ec;
  const fs::path exe_path = fs::read_symlink("/proc/self/exe", ec);
  if (!ec && !exe_path.empty()) {
    return exe_path.parent_path() / "templates";
  }
  return fs::current_path() / "templates";
}

static int64_t NowEpochSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

static Config LoadConfig() {
  Config cfg;
  cfg.bind_addr    = EnvOr("ASMBOX_BIND_ADDR", "0.0.0.0");
  cfg.template_dir = EnvOr("ASMBOX_TEMPLATE_DIR", DefaultTemplateDir().string());
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

static std::string Trim(std::string value) {
  auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
              value.end());
  return value;
}

static bool IsValidUsername(const std::string& username) {
  static const std::regex kUsernameRegex("^[A-Za-z0-9_.-]{3,64}$");
  return std::regex_match(username, kUsernameRegex);
}

static bool IsValidPassword(const std::string& password) {
  return password.size() >= 8;
}

static nlohmann::json ToJson(const storage::UserInfo& user) {
  return {
      {"id", user.id},
      {"username", user.username},
      {"is_admin", user.is_admin},
      {"is_active", user.is_active},
      {"created_at", user.created_at},
      {"last_login_at", user.last_login_at},
  };
}

static int BootstrapAdmin(storage::MetadataStorage* metadata) {
  const std::string username = Trim(EnvOr("ASMBOX_ADMIN_USER", ""));
  const std::string password = EnvOr("ASMBOX_ADMIN_PASSWORD", "");
  if (username.empty() && password.empty()) {
    return EXIT_SUCCESS;
  }
  if (username.empty() || password.empty()) {
    PLOG_WARNING << "ASMBOX_ADMIN_USER and ASMBOX_ADMIN_PASSWORD must both be "
                    "set for admin bootstrap";
    return EXIT_FAILURE;
  }
  if (!IsValidUsername(username)) {
    PLOG_ERROR << "Invalid ASMBOX_ADMIN_USER '" << username << "'";
    return EXIT_FAILURE;
  }
  if (!IsValidPassword(password)) {
    PLOG_ERROR << "ASMBOX_ADMIN_PASSWORD must be at least 8 characters";
    return EXIT_FAILURE;
  }

  const auto existing_user = metadata->GetUserAuthInfo(username);
  if (existing_user) {
    if (existing_user->user.is_admin) {
      PLOG_INFO << "Bootstrap admin already exists: " << username;
      return EXIT_SUCCESS;
    }
    if (!metadata->SetUserAdmin(existing_user->user.id, true)) {
      PLOG_ERROR << "Failed to promote bootstrap admin user: " << username;
      return EXIT_FAILURE;
    }
    PLOG_INFO << "Promoted existing user to admin: " << username;
    return EXIT_SUCCESS;
  }

  const auto password_record = auth::HashPassword(password);
  if (!password_record) {
    PLOG_ERROR << "Failed to hash bootstrap admin password";
    return EXIT_FAILURE;
  }
  if (!metadata->CreateUser(username, *password_record, true)) {
    PLOG_ERROR << "Failed to create bootstrap admin user: " << username;
    return EXIT_FAILURE;
  }
  PLOG_INFO << "Created bootstrap admin user: " << username;
  return EXIT_SUCCESS;
}

static int CreateAdminUser(storage::MetadataStorage* metadata,
                           const std::string& username,
                           const std::string& password) {
  if (!IsValidUsername(username)) {
    std::cerr << "Invalid username. Use 3-64 chars [A-Za-z0-9_.-].\n";
    return EXIT_FAILURE;
  }
  if (!IsValidPassword(password)) {
    std::cerr << "Password must be at least 8 characters.\n";
    return EXIT_FAILURE;
  }

  const auto existing_user = metadata->GetUserAuthInfo(username);
  if (existing_user) {
    if (existing_user->user.is_admin) {
      std::cout << "Admin user already exists: " << username << '\n';
      return EXIT_SUCCESS;
    }
    if (!metadata->SetUserAdmin(existing_user->user.id, true)) {
      std::cerr << "Failed to promote existing user to admin.\n";
      return EXIT_FAILURE;
    }
    std::cout << "Promoted existing user to admin: " << username << '\n';
    return EXIT_SUCCESS;
  }

  const auto password_record = auth::HashPassword(password);
  if (!password_record) {
    std::cerr << "Failed to hash password.\n";
    return EXIT_FAILURE;
  }
  if (!metadata->CreateUser(username, *password_record, true)) {
    std::cerr << "Failed to create admin user.\n";
    return EXIT_FAILURE;
  }
  std::cout << "Created admin user: " << username << '\n';
  return EXIT_SUCCESS;
}

static std::optional<std::string> ReadPasswordForCli() {
  std::string password;
  std::string confirmation;
  std::cout << "Password: ";
  if (!std::getline(std::cin, password)) {
    return std::nullopt;
  }
  std::cout << "Confirm password: ";
  if (!std::getline(std::cin, confirmation)) {
    return std::nullopt;
  }
  if (password != confirmation) {
    std::cerr << "Passwords do not match.\n";
    return std::nullopt;
  }
  return password;
}

static int RunCliCommand(storage::MetadataStorage* metadata, int argc,
                         char* argv[]) {
  if (argc < 2) {
    return -1;
  }

  const std::string command = argv[1];
  if (command != "create-admin") {
    std::cerr << "Unknown command: " << command << '\n';
    return EXIT_FAILURE;
  }
  if (argc < 3) {
    std::cerr << "Usage: asmbox create-admin <username> [--password <password>]\n";
    return EXIT_FAILURE;
  }

  const std::string username = argv[2];
  std::optional<std::string> password;
  for (int i = 3; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--password") {
      if (i + 1 >= argc) {
        std::cerr << "--password requires a value.\n";
        return EXIT_FAILURE;
      }
      password = std::string(argv[++i]);
      continue;
    }
    std::cerr << "Unknown option: " << arg << '\n';
    return EXIT_FAILURE;
  }

  if (!password) {
    const std::string env_password = EnvOr("ASMBOX_ADMIN_PASSWORD", "");
    if (!env_password.empty()) {
      password = env_password;
    } else {
      password = ReadPasswordForCli();
    }
  }
  if (!password) {
    return EXIT_FAILURE;
  }
  return CreateAdminUser(metadata, username, *password);
}

static std::optional<std::string> GetCookieValue(const crow::request& req,
                                                 const std::string& name) {
  const std::string cookie_header = req.get_header_value("Cookie");
  if (cookie_header.empty()) {
    return std::nullopt;
  }

  std::stringstream stream(cookie_header);
  std::string part;
  while (std::getline(stream, part, ';')) {
    part = Trim(part);
    const std::size_t equals = part.find('=');
    if (equals == std::string::npos) {
      continue;
    }
    if (part.substr(0, equals) == name) {
      return part.substr(equals + 1);
    }
  }
  return std::nullopt;
}

static void SetSessionCookie(crow::response* res, const std::string& token) {
  res->add_header(
      "Set-Cookie",
      std::string(kSessionCookieName) + "=" + token +
          "; Path=/; HttpOnly; SameSite=Lax; Max-Age=" +
          std::to_string(kSessionLifetimeSeconds));
}

static void ClearSessionCookie(crow::response* res) {
  res->add_header(
      "Set-Cookie",
      std::string(kSessionCookieName) +
          "=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0");
}

static std::optional<UserContext> AuthenticateRequest(
    const crow::request& req, storage::MetadataStorage* metadata) {
  const auto session_cookie = GetCookieValue(req, kSessionCookieName);
  if (!session_cookie) {
    return std::nullopt;
  }
  const auto token_hash = auth::Sha256Hex(*session_cookie);
  if (!token_hash) {
    return std::nullopt;
  }
  const auto session = metadata->GetSession(*token_hash, NowEpochSeconds());
  if (!session) {
    return std::nullopt;
  }
  UserContext user;
  user.user_id = session->user_id;
  user.username = session->username;
  user.is_admin = session->is_admin;
  user.session_token = *session_cookie;
  return user;
}

static std::optional<storage::WorkspaceRecord> RequireWorkspace(
    storage::MetadataStorage* metadata, const UserContext& user,
    const std::string& workspace_id) {
  return metadata->GetWorkspace(user.user_id, workspace_id);
}

static fs::path WorkspaceRootPath(const storage::WorkspaceRecord& workspace) {
  return fs::path(workspace.root_path);
}

static fs::path WorkspaceFilesPath(const storage::WorkspaceRecord& workspace) {
  return WorkspaceRootPath(workspace) / "files";
}

static fs::path WorkspaceDbPath(const storage::WorkspaceRecord& workspace) {
  return WorkspaceRootPath(workspace) / "workspace.sqlite";
}

static nlohmann::json BuildWorkspaceFileList(
    const storage::WorkspaceRecord& workspace) {
  nlohmann::json file_list = nlohmann::json::array();
  const fs::path files_root = WorkspaceFilesPath(workspace);
  if (!fs::exists(files_root)) {
    return file_list;
  }
  for (const auto& entry : fs::recursive_directory_iterator(files_root)) {
    if (!entry.is_regular_file()) continue;
    std::string ext = entry.path().extension().string();
    if (!utils::EndsWithCI(ext, ".sldprt") &&
        !utils::EndsWithCI(ext, ".sldasm") &&
        !utils::EndsWithCI(ext, ".slddrw")) {
      continue;
    }
    std::string rel = fs::relative(entry.path(), files_root).string();
    file_list.push_back({
        {"name", entry.path().filename().string()},
        {"rel_path", rel},
        {"session", workspace.id},
        {"workspace_name", workspace.display_name},
    });
  }
  return file_list;
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

int main(int argc, char* argv[]) {
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

  fs::path main_db_path = cfg.data_dir / "main.sqlite";
  auto metadata = std::make_shared<storage::MetadataStorage>(main_db_path.string());
  PLOG_INFO << "Main DB initialized: " << main_db_path.string();
  if (argc > 1) {
    return RunCliCommand(metadata.get(), argc, argv);
  }
  if (BootstrapAdmin(metadata.get()) != EXIT_SUCCESS) {
    return EXIT_FAILURE;
  }
  const bool expired_sessions_deleted =
      metadata->DeleteExpiredSessions(NowEpochSeconds());
  (void)expired_sessions_deleted;

  crow::SimpleApp app;

  CROW_ROUTE(app, "/")([]() {
    return crow::mustache::load_text("index.html");
  });

  CROW_ROUTE(app, "/api/auth/register")
      .methods(crow::HTTPMethod::Post)(
          [](const crow::request&) {
            return crow::response(403, "Self-service registration is disabled");
          });

  CROW_ROUTE(app, "/api/admin/users")
      .methods(crow::HTTPMethod::Post)(
          [metadata](const crow::request& req) {
            const auto user = AuthenticateRequest(req, metadata.get());
            if (!user) {
              return crow::response(401);
            }
            if (!user->is_admin) {
              return crow::response(403, "Admin privileges required");
            }
            try {
              const auto body = nlohmann::json::parse(req.body);
              const std::string username = Trim(body.value("username", ""));
              const std::string password = body.value("password", "");
              const bool is_admin = body.value("is_admin", false);
              if (!IsValidUsername(username)) {
                return crow::response(400, "Invalid username");
              }
              if (!IsValidPassword(password)) {
                return crow::response(400, "Password must be at least 8 characters");
              }
              const auto password_record = auth::HashPassword(password);
              if (!password_record) {
                return crow::response(500, "Password hashing failed");
              }
              if (!metadata->CreateUser(username, *password_record, is_admin)) {
                return crow::response(409, "Username already exists");
              }
              const auto created_user = metadata->GetUserAuthInfo(username);
              if (!created_user) {
                return crow::response(500, "User creation verification failed");
              }
              nlohmann::json response_json = ToJson(created_user->user);
              crow::response res(201, response_json.dump());
              res.set_header("Content-Type", "application/json; charset=utf-8");
              return res;
            } catch (...) {
              return crow::response(400, "Invalid JSON");
            }
          });

  CROW_ROUTE(app, "/api/admin/users")
      .methods(crow::HTTPMethod::Get)(
          [metadata](const crow::request& req) {
            const auto user = AuthenticateRequest(req, metadata.get());
            if (!user) {
              return crow::response(401);
            }
            if (!user->is_admin) {
              return crow::response(403, "Admin privileges required");
            }
            nlohmann::json response_json = nlohmann::json::array();
            for (const auto& listed_user : metadata->GetUsers()) {
              response_json.push_back(ToJson(listed_user));
            }
            crow::response res(response_json.dump());
            res.set_header("Content-Type", "application/json; charset=utf-8");
            return res;
          });

  CROW_ROUTE(app, "/api/admin/users/<int>")
      .methods(crow::HTTPMethod::Put)(
          [metadata](const crow::request& req, int user_id_value) {
            const auto user = AuthenticateRequest(req, metadata.get());
            if (!user) {
              return crow::response(401);
            }
            if (!user->is_admin) {
              return crow::response(403, "Admin privileges required");
            }
            const int64_t user_id = user_id_value;
            const auto existing_user = metadata->GetUserById(user_id);
            if (!existing_user) {
              return crow::response(404, "User not found");
            }
            try {
              const auto body = nlohmann::json::parse(req.body);
              const std::string username =
                  Trim(body.value("username", existing_user->username));
              const bool is_admin =
                  body.value("is_admin", existing_user->is_admin);
              const bool is_active =
                  body.value("is_active", existing_user->is_active);
              const std::string password = body.value("password", "");
              if (!IsValidUsername(username)) {
                return crow::response(400, "Invalid username");
              }
              if (!password.empty() && !IsValidPassword(password)) {
                return crow::response(400, "Password must be at least 8 characters");
              }
              if (user_id == user->user_id &&
                  (!is_admin || !is_active)) {
                return crow::response(400, "You cannot deactivate or demote your current account");
              }
              if (existing_user->is_admin && existing_user->is_active &&
                  (!is_admin || !is_active) &&
                  metadata->CountActiveAdmins() <= 1) {
                return crow::response(400, "At least one active admin must remain");
              }

              if (!metadata->UpdateUser(user_id, username, is_admin, is_active)) {
                return crow::response(500, "User update failed");
              }
              if (!password.empty()) {
                const auto password_record = auth::HashPassword(password);
                if (!password_record ||
                    !metadata->UpdateUserPassword(user_id, *password_record)) {
                  return crow::response(500, "Password update failed");
                }
              }

              const auto updated_user = metadata->GetUserById(user_id);
              if (!updated_user) {
                return crow::response(500, "User update verification failed");
              }
              crow::response res(ToJson(*updated_user).dump());
              res.set_header("Content-Type", "application/json; charset=utf-8");
              return res;
            } catch (...) {
              return crow::response(400, "Invalid JSON");
            }
          });

  CROW_ROUTE(app, "/api/admin/users/<int>")
      .methods(crow::HTTPMethod::Delete)(
          [metadata](const crow::request& req, int user_id_value) {
            const auto user = AuthenticateRequest(req, metadata.get());
            if (!user) {
              return crow::response(401);
            }
            if (!user->is_admin) {
              return crow::response(403, "Admin privileges required");
            }
            const int64_t user_id = user_id_value;
            const auto existing_user = metadata->GetUserById(user_id);
            if (!existing_user) {
              return crow::response(404, "User not found");
            }
            if (user_id == user->user_id) {
              return crow::response(400, "You cannot delete your current account");
            }
            if (existing_user->is_admin && existing_user->is_active &&
                metadata->CountActiveAdmins() <= 1) {
              return crow::response(400, "At least one active admin must remain");
            }
            if (!metadata->DeleteUser(user_id)) {
              return crow::response(500, "User deletion failed");
            }
            return crow::response(200);
          });

  CROW_ROUTE(app, "/api/auth/login")
      .methods(crow::HTTPMethod::Post)(
          [metadata](const crow::request& req) {
            try {
              const auto body = nlohmann::json::parse(req.body);
              const std::string username = Trim(body.value("username", ""));
              const std::string password = body.value("password", "");
              const auto user_auth = metadata->GetUserAuthInfo(username);
              if (!user_auth || !user_auth->user.is_active ||
                  !auth::VerifyPassword(password, user_auth->password)) {
                return crow::response(401, "Invalid credentials");
              }
              const auto session_token = auth::GenerateTokenHex(32);
              const auto token_hash =
                  session_token ? auth::Sha256Hex(*session_token) : std::nullopt;
              if (!session_token || !token_hash) {
                return crow::response(500, "Session creation failed");
              }
              const int64_t now = NowEpochSeconds();
              const int64_t expires_at = now + kSessionLifetimeSeconds;
              if (!metadata->CreateSession(user_auth->user.id, *token_hash,
                                           expires_at, now)) {
                return crow::response(500, "Session creation failed");
              }
              const bool login_updated =
                  metadata->UpdateLastLogin(user_auth->user.id, now);
              (void)login_updated;

              nlohmann::json response_json{
                  {"id", user_auth->user.id},
                  {"username", user_auth->user.username},
                  {"is_admin", user_auth->user.is_admin},
              };
              crow::response res(response_json.dump());
              res.set_header("Content-Type", "application/json; charset=utf-8");
              SetSessionCookie(&res, *session_token);
              return res;
            } catch (...) {
              return crow::response(400, "Invalid JSON");
            }
          });

  CROW_ROUTE(app, "/api/auth/logout")
      .methods(crow::HTTPMethod::Post)([metadata](const crow::request& req) {
        if (const auto session_cookie = GetCookieValue(req, kSessionCookieName)) {
          if (const auto token_hash = auth::Sha256Hex(*session_cookie)) {
            const bool deleted = metadata->DeleteSession(*token_hash);
            (void)deleted;
          }
        }
        crow::response res(200);
        ClearSessionCookie(&res);
        return res;
      });

  CROW_ROUTE(app, "/api/auth/me")
      .methods(crow::HTTPMethod::Get)([metadata](const crow::request& req) {
        const auto user = AuthenticateRequest(req, metadata.get());
        if (!user) {
          return crow::response(401);
        }
        const auto user_info = metadata->GetUserById(user->user_id);
        if (!user_info) {
          return crow::response(404);
        }
        nlohmann::json response_json = ToJson(*user_info);
        crow::response res(response_json.dump());
        res.set_header("Content-Type", "application/json; charset=utf-8");
        return res;
      });

  CROW_ROUTE(app, "/api/account")
      .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Put)(
          [metadata](const crow::request& req) {
            const auto user = AuthenticateRequest(req, metadata.get());
            if (!user) {
              return crow::response(401);
            }
            const auto current_user = metadata->GetUserById(user->user_id);
            if (!current_user) {
              return crow::response(404, "User not found");
            }
            if (req.method == crow::HTTPMethod::Get) {
              crow::response res(ToJson(*current_user).dump());
              res.set_header("Content-Type", "application/json; charset=utf-8");
              return res;
            }

            try {
              const auto body = nlohmann::json::parse(req.body);
              const std::string username =
                  Trim(body.value("username", current_user->username));
              const std::string current_password =
                  body.value("current_password", "");
              const std::string new_password = body.value("new_password", "");
              if (!IsValidUsername(username)) {
                return crow::response(400, "Invalid username");
              }
              if (current_password.empty()) {
                return crow::response(400, "Current password is required");
              }
              if (!new_password.empty() && !IsValidPassword(new_password)) {
                return crow::response(400, "New password must be at least 8 characters");
              }

              const auto auth_info = metadata->GetUserAuthInfo(user->username);
              if (!auth_info ||
                  !auth::VerifyPassword(current_password, auth_info->password)) {
                return crow::response(401, "Current password is incorrect");
              }

              if (!metadata->UpdateUser(user->user_id, username,
                                        current_user->is_admin,
                                        current_user->is_active)) {
                return crow::response(500, "Profile update failed");
              }
              if (!new_password.empty()) {
                const auto password_record = auth::HashPassword(new_password);
                if (!password_record ||
                    !metadata->UpdateUserPassword(user->user_id,
                                                  *password_record)) {
                  return crow::response(500, "Password update failed");
                }
              }

              const auto updated_user = metadata->GetUserById(user->user_id);
              if (!updated_user) {
                return crow::response(500, "Profile update verification failed");
              }
              crow::response res(ToJson(*updated_user).dump());
              res.set_header("Content-Type", "application/json; charset=utf-8");
              return res;
            } catch (...) {
              return crow::response(400, "Invalid JSON");
            }
          });

  CROW_ROUTE(app, "/api/workspaces")
      .methods(crow::HTTPMethod::Get)([metadata](const crow::request& req) {
        const auto user = AuthenticateRequest(req, metadata.get());
        if (!user) {
          return crow::response(401);
        }
        auto ws = metadata->GetWorkspaces(user->user_id);
        nlohmann::json j = nlohmann::json::array();
        for (const auto& w : ws)
          j.push_back({{"id", w.id},
                       {"name", w.display_name},
                       {"created_at", w.created_at}});
        crow::response res(j.dump());
        res.set_header("Content-Type", "application/json; charset=utf-8");
        return res;
      });

  CROW_ROUTE(app, "/api/workspaces/<string>")
      .methods(crow::HTTPMethod::Delete)(
          [metadata](const crow::request& req, std::string workspace_id) {
            const auto user = AuthenticateRequest(req, metadata.get());
            if (!user) {
              return crow::response(401);
            }
            const auto workspace =
                RequireWorkspace(metadata.get(), *user, workspace_id);
            if (!workspace) {
              return crow::response(404);
            }
            DeleteDirectory(WorkspaceRootPath(*workspace));
            const bool deleted =
                metadata->DeleteWorkspace(user->user_id, workspace_id);
            (void)deleted;
            return crow::response(200);
          });

  CROW_ROUTE(app, "/api/workspaces/<string>/files")
      .methods(crow::HTTPMethod::Get)(
          [metadata](const crow::request& req, std::string workspace_id) {
            const auto user = AuthenticateRequest(req, metadata.get());
            if (!user) {
              return crow::response(401);
            }
            const auto workspace =
                RequireWorkspace(metadata.get(), *user, workspace_id);
            if (!workspace) {
              return crow::response(404);
            }
            crow::response res(BuildWorkspaceFileList(*workspace).dump());
            res.set_header("Content-Type", "application/json; charset=utf-8");
            return res;
          });

  CROW_ROUTE(app, "/upload")
      .methods(crow::HTTPMethod::Post)(
          [metadata, data_dir = cfg.data_dir](const crow::request& req) {
            try {
              const auto user = AuthenticateRequest(req, metadata.get());
              if (!user) {
                return crow::response(401);
              }
              crow::multipart::message msg(req);
              auto zip_part = msg.get_part_by_name("file");
              if (zip_part.body.empty())
                return crow::response(400);

              std::string filename = "Upload";
              auto disp =
                  zip_part.get_header_object("Content-Disposition").params;
              if (disp.find("filename") != disp.end())
                filename = disp.at("filename");

              std::string display_name = filename;
              auto dot = display_name.find_last_of('.');
              if (dot != std::string::npos) {
                display_name = display_name.substr(0, dot);
              }
              if (display_name.empty()) {
                display_name = "Upload";
              }
              const auto workspace_id = auth::GenerateWorkspaceId();
              if (!workspace_id) {
                return crow::response(500, "Workspace ID generation failed");
              }
              fs::path workspace_root =
                  data_dir / "users" / std::to_string(user->user_id) /
                  "workspaces" / *workspace_id;
              fs::path files_root = workspace_root / "files";
              DeleteDirectory(workspace_root);
              fs::create_directories(files_root);

              const utils::ZipExtractionResult unzip_result =
                  utils::UnzipToFolder(zip_part.body, files_root);
              if (!unzip_result.ok()) {
                PLOG_WARNING << "Rejected ZIP upload: " << unzip_result.error();
                DeleteDirectory(workspace_root);
                return crow::response(400, unzip_result.error());
              }

              if (!metadata->CreateWorkspace(
                      user->user_id, *workspace_id, display_name,
                      workspace_root.string())) {
                DeleteDirectory(workspace_root);
                return crow::response(500, "DB error");
              }
              storage::SqliteStorage workspace_storage(
                  WorkspaceDbPath({*workspace_id, user->user_id, display_name,
                                   workspace_root.string(), ""})
                      .string());

              // Enumerate SolidWorks files and eagerly analyze each one so the
              // property index is populated immediately after upload.
              for (const auto& entry :
                   fs::recursive_directory_iterator(files_root)) {
                if (!entry.is_regular_file()) continue;
                std::string ext = entry.path().extension().string();
                if (!utils::EndsWithCI(ext, ".sldprt") &&
                    !utils::EndsWithCI(ext, ".sldasm") &&
                    !utils::EndsWithCI(ext, ".slddrw"))
                  continue;

                std::string rel = fs::relative(entry.path(), files_root).string();
                // Analyze and cache in SQLite if not already there.
                nlohmann::json probe;
                if (!workspace_storage.Load(rel, &probe)) {
                  nlohmann::json doc_data;
                  sw::DocumentAnalyzer analyzer;
                  PLOG_INFO << "Scanning: " << rel;
                  if (analyzer.AnalyzeFile(entry.path(), &doc_data))
                    workspace_storage.Save(rel, doc_data);
                  else
                    PLOG_WARNING << "Scan failed: " << rel;
                }
              }
              const storage::WorkspaceRecord workspace_record{
                  *workspace_id, user->user_id, display_name,
                  workspace_root.string(), ""};
              nlohmann::json file_list = BuildWorkspaceFileList(workspace_record);
              crow::response res(file_list.dump());
              res.set_header("Content-Type", "application/json; charset=utf-8");
              return res;
            } catch (const std::exception& e) {
              PLOG_FATAL << e.what();
              return crow::response(500);
            }
          });

  CROW_ROUTE(app, "/api/doc/<string>/<string>")
      ([metadata](const crow::request& req, std::string workspace_id,
                  std::string path_b64) {
        try {
          const auto user = AuthenticateRequest(req, metadata.get());
          if (!user) {
            return crow::response(401);
          }
          const auto workspace =
              RequireWorkspace(metadata.get(), *user, workspace_id);
          if (!workspace) {
            return crow::response(404, "Workspace not found");
          }
          std::string decoded_path = utils::Base64UrlDecode(path_b64);
          if (decoded_path.find("..") != std::string::npos)
            return crow::response(400, "Invalid path");

          storage::SqliteStorage workspace_storage(
              WorkspaceDbPath(*workspace).string());
          fs::path f_path = WorkspaceFilesPath(*workspace) / decoded_path;

          nlohmann::json data;
          if (workspace_storage.Load(decoded_path, &data)) {
            PLOG_INFO << "API doc cache hit: " << decoded_path;
          } else {
            if (!fs::exists(f_path))
              return crow::response(404, "File not found");
            PLOG_INFO << "API doc analyzing: " << decoded_path;
            sw::DocumentAnalyzer analyzer;
            if (!analyzer.AnalyzeFile(f_path, &data))
              return crow::response(500, "Analysis failed");
            workspace_storage.Save(decoded_path, data);
          }

          crow::response res(data.dump());
          res.set_header("Content-Type", "application/json; charset=utf-8");
          return res;
        } catch (const std::exception& e) {
          PLOG_FATAL << e.what();
          return crow::response(500);
        }
      });

  CROW_ROUTE(app, "/api/workspaces/<string>/props")
      .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Post)(
          [metadata](const crow::request& req, std::string workspace_id) {
            const auto user = AuthenticateRequest(req, metadata.get());
            if (!user) {
              return crow::response(401);
            }
            const auto workspace =
                RequireWorkspace(metadata.get(), *user, workspace_id);
            if (!workspace) {
              return crow::response(404);
            }
            storage::SqliteStorage workspace_storage(
                WorkspaceDbPath(*workspace).string());
            if (req.method == crow::HTTPMethod::Post) {
              try {
                auto body = nlohmann::json::parse(req.body);
                if (!workspace_storage.SetPropertyConfig(body))
                  return crow::response(500, "DB error");
                return crow::response(200);
              } catch (...) {
                return crow::response(400, "Invalid JSON");
              }
            }

            // GET
            auto names = workspace_storage.GetPropertyNames();
            auto configs = workspace_storage.GetPropertyConfig();

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

  CROW_ROUTE(app, "/api/profiles")
      .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Post)(
          [metadata](const crow::request& req) {
            const auto user = AuthenticateRequest(req, metadata.get());
            if (!user) {
              return crow::response(401);
            }
            if (req.method == crow::HTTPMethod::Post) {
              try {
                auto body = nlohmann::json::parse(req.body);
                std::string name = body.value("name", "");
                std::string desc = body.value("description", "");
                if (name.empty()) return crow::response(400, "name required");
                int64_t id =
                    metadata->CreateProfile(user->user_id, name, desc);
                if (id < 0) return crow::response(409, "Name already exists");
                nlohmann::json j; j["id"] = id;
                return crow::response(j.dump());
              } catch (...) { return crow::response(400, "Invalid JSON"); }
            }
            // GET
            crow::response res(metadata->GetProfiles(user->user_id).dump());
            res.set_header("Content-Type", "application/json; charset=utf-8");
            return res;
          });

  CROW_ROUTE(app, "/api/profiles/<int>")
      .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Put,
               crow::HTTPMethod::Delete)(
          [metadata](const crow::request& req, int profile_id) {
            const auto user = AuthenticateRequest(req, metadata.get());
            if (!user) {
              return crow::response(401);
            }
            if (req.method == crow::HTTPMethod::Delete) {
              return metadata->DeleteProfile(user->user_id, profile_id)
                  ? crow::response(200) : crow::response(404);
            }
            if (req.method == crow::HTTPMethod::Put) {
              try {
                auto body = nlohmann::json::parse(req.body);
                body["id"] = profile_id;
                return metadata->SaveProfile(user->user_id, body)
                    ? crow::response(200) : crow::response(400, "Save failed");
              } catch (...) { return crow::response(400, "Invalid JSON"); }
            }
            // GET
            auto p = metadata->GetProfile(user->user_id, profile_id);
            if (p.is_null() || p.empty()) return crow::response(404);
            crow::response res(p.dump());
            res.set_header("Content-Type", "application/json; charset=utf-8");
            return res;
          });

  CROW_ROUTE(app, "/api/bom/<string>/<string>")
      ([metadata](const crow::request& req, std::string workspace_id,
                                           std::string path_b64) {
        try {
          const auto user = AuthenticateRequest(req, metadata.get());
          if (!user) {
            return crow::response(401);
          }
          const auto workspace =
              RequireWorkspace(metadata.get(), *user, workspace_id);
          if (!workspace) {
            return crow::response(404, "Workspace not found");
          }
          std::string decoded_path = utils::Base64UrlDecode(path_b64);
          if (decoded_path.find("..") != std::string::npos)
            return crow::response(400, "Invalid path");

          storage::SqliteStorage workspace_storage(
              WorkspaceDbPath(*workspace).string());
          fs::path files_root = WorkspaceFilesPath(*workspace);
          fs::path f_path = files_root / decoded_path;
          if (!fs::exists(f_path))
            return crow::response(404, "File not found");

          PLOG_INFO << "Building BOM: " << decoded_path;

          openbom::BomTransformerConfig bom_cfg;
          bom_cfg.path_resolver.search_dirs.push_back(files_root);
          bom_cfg.path_resolver.recursive_search = true;
          bom_cfg.path_resolver.case_insensitive = true;

          openbom::BomTransformer transformer(bom_cfg);
          auto result = transformer.Build(f_path);
          if (!result.ok()) {
            return crow::response(422, result.error());
          }

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
              auto profile = metadata->GetProfile(user->user_id, pid);
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
              std::string pn_prop = workspace_storage.GetPartNumberProp();
              std::string cfg_name = bom_json["bom"].value("configuration", "");
              workspace_storage.SaveBom(decoded_path, cfg_name, pn_prop,
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
