#include "lakehouse/lakehouse_catalog.h"

#include <cctype>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "duckdb.hpp"
#include "httplib.hpp"

namespace myduck::lakehouse {
namespace {

std::string MakeTableKey(std::string_view schema_name,
                         std::string_view table_name) {
  std::string key;
  key.reserve(schema_name.size() + 1 + table_name.size());
  key.append(schema_name);
  key.push_back('\0');
  key.append(table_name);
  return key;
}

std::string QuoteSqlIdentifier(std::string_view identifier) {
  std::string out;
  out.reserve(identifier.size() + 4);
  out.push_back('"');
  for (char c : identifier) {
    if (c == '"') {
      out.push_back('"');
    }
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

std::string EscapeSqlLiteral(std::string_view val) {
  std::string out;
  out.reserve(val.size());
  for (char c : val) {
    if (c == '\'') {
      out.append("''");
    } else {
      out.push_back(c);
    }
  }
  return out;
}

bool SplitEndpointUrl(std::string_view raw_endpoint,
                      std::string *scheme_host_port,
                      std::string *base_path) {
  while (!raw_endpoint.empty() && raw_endpoint.back() == '/') {
    raw_endpoint.remove_suffix(1);
  }
  if (raw_endpoint.empty()) {
    return false;
  }

  std::string normalized(raw_endpoint);
  size_t scheme_pos = normalized.find("://");
  if (scheme_pos == std::string::npos) {
    normalized = "http://" + normalized;
    scheme_pos = 4;
  }

  size_t host_start = scheme_pos + 3;
  if (host_start >= normalized.size()) {
    return false;
  }

  size_t slash_pos = normalized.find('/', host_start);
  if (slash_pos == std::string::npos) {
    *scheme_host_port = normalized;
    base_path->clear();
  } else {
    *scheme_host_port = normalized.substr(0, slash_pos);
    *base_path = normalized.substr(slash_pos);
  }
  return !scheme_host_port->empty();
}

bool ParseJsonStringLiteral(std::string_view json, size_t *pos_inout,
                            std::string *out) {
  size_t pos = *pos_inout;
  if (pos >= json.size() || json[pos] != '"') {
    return false;
  }
  ++pos;

  std::string result;
  while (pos < json.size()) {
    char c = json[pos++];
    if (c == '"') {
      *pos_inout = pos;
      if (out != nullptr) {
        *out = std::move(result);
      }
      return true;
    }
    if (c == '\\') {
      if (pos >= json.size()) {
        return false;
      }
      char esc = json[pos++];
      switch (esc) {
        case '"':
        case '\\':
        case '/':
          result.push_back(esc);
          break;
        case 'b':
          result.push_back('\b');
          break;
        case 'f':
          result.push_back('\f');
          break;
        case 'n':
          result.push_back('\n');
          break;
        case 'r':
          result.push_back('\r');
          break;
        case 't':
          result.push_back('\t');
          break;
        case 'u': {
          if (pos + 4 > json.size()) {
            return false;
          }
          unsigned int codepoint = 0;
          for (size_t i = 0; i < 4; ++i) {
            char hc = json[pos + i];
            codepoint <<= 4;
            if (hc >= '0' && hc <= '9') {
              codepoint |= static_cast<unsigned int>(hc - '0');
            } else if (hc >= 'a' && hc <= 'f') {
              codepoint |= static_cast<unsigned int>(10 + (hc - 'a'));
            } else if (hc >= 'A' && hc <= 'F') {
              codepoint |= static_cast<unsigned int>(10 + (hc - 'A'));
            } else {
              return false;
            }
          }
          pos += 4;
          if (codepoint <= 0x7F) {
            result.push_back(static_cast<char>(codepoint));
          }
          break;
        }
        default:
          result.push_back(esc);
          break;
      }
    } else {
      result.push_back(c);
    }
  }
  return false;
}

bool ExtractJsonStringField(std::string_view json, std::string_view key,
                            std::string *value_out) {
  size_t pos = 0;
  while (pos < json.size()) {
    if (json[pos] != '"') {
      ++pos;
      continue;
    }

    std::string token;
    if (!ParseJsonStringLiteral(json, &pos, &token)) {
      return false;
    }

    while (pos < json.size() &&
           std::isspace(static_cast<unsigned char>(json[pos]))) {
      ++pos;
    }

    // Only match when the string literal is followed by ':' (an object key).
    if (pos >= json.size() || json[pos] != ':') {
      continue;
    }
    ++pos;

    while (pos < json.size() &&
           std::isspace(static_cast<unsigned char>(json[pos]))) {
      ++pos;
    }

    if (token == key) {
      if (pos < json.size() && json[pos] == '"') {
        std::string val;
        if (!ParseJsonStringLiteral(json, &pos, &val)) {
          return false;
        }
        if (value_out != nullptr) {
          *value_out = std::move(val);
        }
        return true;
      }
    } else if (pos < json.size() && json[pos] == '"') {
      // Consume the string value so it is never mistaken for a key token.
      if (!ParseJsonStringLiteral(json, &pos, nullptr)) {
        return false;
      }
    }
  }
  return false;
}

}  // namespace

LakehouseCatalog &LakehouseCatalog::Instance() {
  static LakehouseCatalog instance;
  return instance;
}

void LakehouseCatalog::RegisterTable(const LakehouseTableConfig &config) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  tables_[MakeTableKey(config.schema_name, config.table_name)] = config;
}

void LakehouseCatalog::UnregisterTable(std::string_view schema_name,
                                       std::string_view table_name) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  tables_.erase(MakeTableKey(schema_name, table_name));
}

void LakehouseCatalog::UnregisterSchema(std::string_view schema_name) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  for (auto it = tables_.begin(); it != tables_.end();) {
    if (it->second.schema_name == schema_name) {
      it = tables_.erase(it);
    } else {
      ++it;
    }
  }
}

void LakehouseCatalog::RenameTable(std::string_view old_schema,
                                   std::string_view old_table,
                                   std::string_view new_schema,
                                   std::string_view new_table) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  auto it = tables_.find(MakeTableKey(old_schema, old_table));
  if (it == tables_.end()) {
    return;
  }
  LakehouseTableConfig updated = std::move(it->second);
  tables_.erase(it);
  updated.schema_name = std::string(new_schema);
  updated.table_name = std::string(new_table);
  tables_[MakeTableKey(new_schema, new_table)] = std::move(updated);
}

bool LakehouseCatalog::IsLakehouseTable(std::string_view schema_name,
                                        std::string_view table_name) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return tables_.find(MakeTableKey(schema_name, table_name)) != tables_.end();
}

std::optional<LakehouseTableConfig> LakehouseCatalog::FindTable(
    std::string_view schema_name, std::string_view table_name) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  auto it = tables_.find(MakeTableKey(schema_name, table_name));
  if (it == tables_.end()) {
    return std::nullopt;
  }
  return it->second;
}

bool LakehouseCatalog::AttachOrResolveIcebergRest(
    duckdb::Connection &conn, const LakehouseTableConfig &config,
    std::string *resolved_scan_sql, std::string *error_out) {
  (void)conn;
  auto set_error = [&](std::string msg) {
    if (error_out != nullptr) {
      *error_out = std::move(msg);
    }
    return false;
  };

  if (resolved_scan_sql == nullptr) {
    return set_error("Iceberg REST resolver requires non-null resolved_scan_sql output pointer");
  }
  if (config.rest_endpoint.empty() || config.rest_namespace.empty() ||
      config.rest_table.empty()) {
    return set_error(
        "Iceberg REST catalog configuration is missing endpoint, namespace, or table");
  }

  std::string scheme_host_port;
  std::string base_path;
  if (!SplitEndpointUrl(config.rest_endpoint, &scheme_host_port, &base_path)) {
    return set_error("Invalid Iceberg REST catalog endpoint: " +
                     config.rest_endpoint);
  }

  std::string last_failure = "no candidate path succeeded";
  try {
    CPPHTTPLIB_NAMESPACE::Client client(scheme_host_port);
    client.set_follow_location(true);
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(5, 0);
    client.set_write_timeout(5, 0);
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    client.enable_server_certificate_verification(false);
#endif

    CPPHTTPLIB_NAMESPACE::Headers headers = {{"Accept", "application/json"}};

    // Optional Iceberg REST Catalog configuration handshake (/v1/config).
    std::string config_path = base_path + "/v1/config";
    if (!config.rest_warehouse.empty()) {
      config_path += "?warehouse=" + config.rest_warehouse;
    }
    std::string discovered_prefix;
    if (auto cfg_res = client.Get(config_path, headers)) {
      if (cfg_res->status == 200) {
        ExtractJsonStringField(cfg_res->body, "prefix", &discovered_prefix);
      }
    }

    std::vector<std::string> candidate_paths;
    if (!discovered_prefix.empty()) {
      candidate_paths.push_back(base_path + "/v1/prefixes/" +
                                discovered_prefix + "/namespaces/" +
                                config.rest_namespace + "/tables/" +
                                config.rest_table);
    }
    candidate_paths.push_back(base_path + "/v1/namespaces/" +
                              config.rest_namespace + "/tables/" +
                              config.rest_table);
    if (!config.rest_warehouse.empty() &&
        config.rest_warehouse != discovered_prefix) {
      candidate_paths.push_back(
          base_path + "/v1/prefixes/" + config.rest_warehouse +
          "/namespaces/" + config.rest_namespace + "/tables/" +
          config.rest_table);
    }

    for (const auto &path : candidate_paths) {
      auto res = client.Get(path, headers);
      if (!res) {
        last_failure = "HTTP GET " + path + " failed: " +
                       CPPHTTPLIB_NAMESPACE::to_string(res.error());
        continue;
      }
      if (res->status != 200) {
        last_failure = "HTTP GET " + path + " returned status " +
                       std::to_string(res->status);
        continue;
      }

      std::string metadata_location;
      if (!ExtractJsonStringField(res->body, "metadata-location",
                                  &metadata_location) &&
          !ExtractJsonStringField(res->body, "metadata_location",
                                  &metadata_location)) {
        last_failure = "Iceberg REST response from " + path +
                       " did not contain 'metadata-location'";
        continue;
      }
      if (metadata_location.empty()) {
        last_failure = "Iceberg REST response from " + path +
                       " returned an empty 'metadata-location'";
        continue;
      }

      std::string scan_path = metadata_location;
      size_t meta_dir_pos = scan_path.rfind("/metadata/");
      if (meta_dir_pos != std::string::npos &&
          scan_path.find('/', meta_dir_pos + 10) == std::string::npos) {
        scan_path.erase(meta_dir_pos);
      }

      std::string path_err;
      if (!ValidateLakehousePath(scan_path, /*allow_remote=*/true, &path_err)) {
        return set_error("Invalid Iceberg REST metadata-location '" +
                         metadata_location + "': " + path_err);
      }

      std::string scan_sql =
          "iceberg_scan('" + EscapeSqlLiteral(scan_path) +
          "', allow_moved_paths := " +
          (config.allow_moved_paths ? "true" : "false");
      if (config.iceberg_version.has_value() &&
          !config.iceberg_version->empty()) {
        scan_sql +=
            ", version := '" + EscapeSqlLiteral(*config.iceberg_version) + "'";
      }
      scan_sql += ")";
      *resolved_scan_sql = std::move(scan_sql);
      if (error_out != nullptr) {
        error_out->clear();
      }
      return true;
    }
  } catch (const std::exception &e) {
    return set_error(std::string("Iceberg REST client exception for '") +
                     config.rest_endpoint + "': " + e.what());
  }

  return set_error("Failed to resolve Iceberg REST table '" +
                   config.rest_namespace + "." + config.rest_table + "' at '" +
                   config.rest_endpoint + "': " + last_failure);
}

void LakehouseCatalog::ClearForTesting() {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  tables_.clear();
}

bool IsDuckDBView(duckdb::Connection &conn, std::string_view schema_name,
                  std::string_view table_name) {
  std::string sql =
      "SELECT 1 FROM duckdb_views() WHERE schema_name = '" +
      EscapeSqlLiteral(schema_name) + "' AND view_name = '" +
      EscapeSqlLiteral(table_name) + "' AND NOT internal LIMIT 1;";
  auto res = conn.Query(sql);
  if (!res || res->HasError()) {
    return false;
  }
  auto chunk = res->Fetch();
  return chunk != nullptr && chunk->size() > 0;
}

bool ExecuteLakehouseCreateTable(
    duckdb::Connection &conn, const LakehouseTableConfig &cfg,
    const std::vector<LakehouseColumnDecl> &cols, std::string *error_out) {
  auto set_error = [&](std::string msg) {
    if (error_out != nullptr) {
      *error_out = std::move(msg);
    }
    return false;
  };

  if (cfg.format == LakehouseFormat::kInvalid ||
      cfg.format == LakehouseFormat::kNone) {
    return set_error(cfg.error_message.empty()
                         ? "Invalid or non-lakehouse table configuration"
                         : cfg.error_message);
  }
  if (cols.empty()) {
    return set_error("Lakehouse table must declare at least one column");
  }

  std::string ddl = TranslateLakehouseCreateView(cfg, cols);
  if (ddl.empty()) {
    return set_error("Failed to translate lakehouse view DDL");
  }

  if (cfg.format == LakehouseFormat::kIcebergRest && cfg.table_path.empty()) {
    std::string resolved_scan_sql;
    if (!LakehouseCatalog::Instance().AttachOrResolveIcebergRest(
            conn, cfg, &resolved_scan_sql, error_out)) {
      return false;
    }
    std::string placeholder = "\nFROM " + BuildDuckDBScanSql(cfg) + ";";
    size_t pos = ddl.rfind(placeholder);
    if (pos != std::string::npos) {
      ddl.replace(pos, placeholder.size(), "\nFROM " + resolved_scan_sql + ";");
    }
  }

  auto res = conn.Query(ddl);
  if (!res || res->HasError()) {
    return set_error(res ? res->GetError()
                         : "DuckDB CREATE VIEW query returned null result");
  }

  LakehouseCatalog::Instance().RegisterTable(cfg);
  if (error_out != nullptr) {
    error_out->clear();
  }
  return true;
}

bool ExecuteLakehouseDropTable(duckdb::Connection &conn,
                               std::string_view schema_name,
                               std::string_view table_name,
                               std::string *error_out) {
  bool is_lakehouse =
      LakehouseCatalog::Instance().IsLakehouseTable(schema_name, table_name) ||
      IsDuckDBView(conn, schema_name, table_name);
  if (is_lakehouse) {
    std::string drop_sql = "DROP VIEW IF EXISTS " +
                           QuoteSqlIdentifier(schema_name) + "." +
                           QuoteSqlIdentifier(table_name) + ";";
    auto res = conn.Query(drop_sql);
    if (!res || res->HasError()) {
      if (error_out != nullptr) {
        *error_out = res ? res->GetError()
                         : "DuckDB DROP VIEW query returned null result";
      }
      return false;
    }
  }
  LakehouseCatalog::Instance().UnregisterTable(schema_name, table_name);
  if (error_out != nullptr) {
    error_out->clear();
  }
  return true;
}

}  // namespace myduck::lakehouse
