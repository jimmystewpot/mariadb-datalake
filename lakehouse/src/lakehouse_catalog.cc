#include "lakehouse/lakehouse_catalog.h"

#include <cctype>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

bool ExtractJsonStringField(std::string_view json, std::string_view key,
                            std::string *value_out) {
  std::string quoted_key;
  quoted_key.reserve(key.size() + 2);
  quoted_key.push_back('"');
  quoted_key.append(key);
  quoted_key.push_back('"');

  size_t pos = json.find(quoted_key);
  if (pos == std::string_view::npos) {
    return false;
  }
  pos += quoted_key.size();

  while (pos < json.size() &&
         std::isspace(static_cast<unsigned char>(json[pos]))) {
    ++pos;
  }
  if (pos >= json.size() || json[pos] != ':') {
    return false;
  }
  ++pos;

  while (pos < json.size() &&
         std::isspace(static_cast<unsigned char>(json[pos]))) {
    ++pos;
  }
  if (pos >= json.size() || json[pos] != '"') {
    return false;
  }
  ++pos;

  std::string result;
  while (pos < json.size()) {
    char c = json[pos++];
    if (c == '"') {
      if (value_out != nullptr) {
        *value_out = std::move(result);
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

  duckdb_httplib::Client client(scheme_host_port);
  client.set_follow_location(true);
  client.set_connection_timeout(5, 0);
  client.set_read_timeout(5, 0);
  client.set_write_timeout(5, 0);

  duckdb_httplib::Headers headers = {{"Accept", "application/json"}};

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
    candidate_paths.push_back(base_path + "/v1/prefixes/" + discovered_prefix +
                              "/namespaces/" + config.rest_namespace +
                              "/tables/" + config.rest_table);
  }
  candidate_paths.push_back(base_path + "/v1/namespaces/" +
                            config.rest_namespace + "/tables/" +
                            config.rest_table);
  if (!config.rest_warehouse.empty() &&
      config.rest_warehouse != discovered_prefix) {
    candidate_paths.push_back(
        base_path + "/v1/prefixes/" + config.rest_warehouse + "/namespaces/" +
        config.rest_namespace + "/tables/" + config.rest_table);
  }

  std::string last_failure = "no candidate path succeeded";
  for (const auto &path : candidate_paths) {
    auto res = client.Get(path, headers);
    if (!res) {
      last_failure = "HTTP GET " + path + " failed: " +
                     duckdb_httplib::to_string(res.error());
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

    *resolved_scan_sql =
        "iceberg_scan('" + EscapeSqlLiteral(scan_path) +
        "', allow_moved_paths := " +
        (config.allow_moved_paths ? "true" : "false") + ")";
    if (error_out != nullptr) {
      error_out->clear();
    }
    return true;
  }

  return set_error("Failed to resolve Iceberg REST table '" +
                   config.rest_namespace + "." + config.rest_table + "' at '" +
                   config.rest_endpoint + "': " + last_failure);
}

void LakehouseCatalog::ClearForTesting() {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  tables_.clear();
}

}  // namespace myduck::lakehouse
