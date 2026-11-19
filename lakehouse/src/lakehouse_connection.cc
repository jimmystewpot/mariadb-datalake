#include "lakehouse/lakehouse_connection.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace myduck::lakehouse {
namespace {

std::mutex g_allowed_prefix_mutex;
std::string g_allowed_path_prefix;

std::string_view TrimWhitespace(std::string_view sv) {
  while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) {
    sv.remove_prefix(1);
  }
  while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) {
    sv.remove_suffix(1);
  }
  return sv;
}

std::string ToLowerAscii(std::string_view sv) {
  std::string out(sv);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

bool StartsWithCaseInsensitive(std::string_view str, std::string_view prefix) {
  if (str.size() < prefix.size()) {
    return false;
  }
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(str[i])) !=
        std::tolower(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

bool HasLakehouseScheme(std::string_view sv) {
  return StartsWithCaseInsensitive(sv, "paimon://") ||
         StartsWithCaseInsensitive(sv, "iceberg://") ||
         StartsWithCaseInsensitive(sv, "iceberg_rest://");
}

bool IsRemoteObjectStoreUri(std::string_view path) {
  return StartsWithCaseInsensitive(path, "s3://") ||
         StartsWithCaseInsensitive(path, "s3a://") ||
         StartsWithCaseInsensitive(path, "gs://") ||
         StartsWithCaseInsensitive(path, "gcs://") ||
         StartsWithCaseInsensitive(path, "az://") ||
         StartsWithCaseInsensitive(path, "abfs://") ||
         StartsWithCaseInsensitive(path, "abfss://") ||
         StartsWithCaseInsensitive(path, "http://") ||
         StartsWithCaseInsensitive(path, "https://") ||
         StartsWithCaseInsensitive(path, "hdfs://") ||
         StartsWithCaseInsensitive(path, "r2://");
}

bool ContainsPathTraversalSegment(std::string_view path) {
  size_t pos = 0;
  while (pos <= path.size()) {
    size_t next = path.find_first_of("/\\", pos);
    std::string_view seg =
        (next == std::string_view::npos) ? path.substr(pos)
                                         : path.substr(pos, next - pos);
    if (seg == "..") {
      return true;
    }
    if (next == std::string_view::npos) {
      break;
    }
    pos = next + 1;
  }
  return false;
}

bool IsForbiddenCredentialParam(std::string_view key) {
  std::string lower = ToLowerAscii(key);
  return lower == "password" || lower == "secret" || lower == "token" ||
         lower == "access_key" || lower == "secret_key" ||
         lower == "session_token" ||
         lower.find("password") != std::string::npos ||
         lower.find("secret") != std::string::npos ||
         lower.find("token") != std::string::npos ||
         lower.find("access_key") != std::string::npos;
}

std::string GetAllowedPathPrefix() {
  std::lock_guard<std::mutex> lock(g_allowed_prefix_mutex);
  return g_allowed_path_prefix;
}

bool IsWithinAllowedPrefix(std::string_view table_path,
                           std::string_view allowed_prefix) {
  std::string_view prefix = TrimWhitespace(allowed_prefix);
  if (prefix.empty()) {
    return true;
  }
  while (prefix.size() > 1 && prefix.back() == '/') {
    prefix.remove_suffix(1);
  }
  if (table_path == prefix) {
    return true;
  }
  if (prefix == "/") {
    return !table_path.empty() && table_path.front() == '/';
  }
  return table_path.size() > prefix.size() &&
         table_path.substr(0, prefix.size()) == prefix &&
         table_path[prefix.size()] == '/';
}

bool ValidateTablePath(std::string_view raw_path, std::string *normalized_out,
                       std::string *error_out) {
  std::string_view path = TrimWhitespace(raw_path);
  if (path.empty()) {
    *error_out = "Lakehouse table path cannot be empty";
    return false;
  }
  if (ContainsPathTraversalSegment(path)) {
    *error_out =
        "Path traversal ('..') is not allowed in lakehouse CONNECTION URI";
    return false;
  }
  const bool is_remote = IsRemoteObjectStoreUri(path);
  if (!is_remote && path.front() != '/') {
    *error_out =
        "Local lakehouse table path must be an absolute path starting with '/'";
    return false;
  }

  std::string clean_path(path);
  while (clean_path.size() > 1 && clean_path.back() == '/' &&
         clean_path.find("://") + 3 != clean_path.size()) {
    clean_path.pop_back();
  }

  std::string allowed_prefix = GetAllowedPathPrefix();
  if (!allowed_prefix.empty() &&
      !IsWithinAllowedPrefix(clean_path, allowed_prefix)) {
    *error_out = "Lakehouse path '" + clean_path +
                 "' is outside the allowed path prefix '" + allowed_prefix +
                 "'";
    return false;
  }

  *normalized_out = std::move(clean_path);
  return true;
}

std::string EscapeSqlLiteral(std::string_view literal) {
  std::string out;
  out.reserve(literal.size() + 4);
  for (char c : literal) {
    if (c == '\'') {
      out.push_back('\'');
    }
    out.push_back(c);
  }
  return out;
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

std::string StripCollateClause(std::string_view duckdb_type) {
  std::string_view trimmed = TrimWhitespace(duckdb_type);
  std::string lower = ToLowerAscii(trimmed);
  size_t pos = lower.find(" collate ");
  if (pos != std::string::npos) {
    trimmed = TrimWhitespace(trimmed.substr(0, pos));
  }
  return std::string(trimmed);
}

}  // namespace

void SetAllowedPathPrefix(std::string prefix) {
  std::lock_guard<std::mutex> lock(g_allowed_prefix_mutex);
  g_allowed_path_prefix = std::string(TrimWhitespace(prefix));
}

LakehouseTableConfig ParseLakehouseUri(std::string_view schema_name,
                                       std::string_view table_name,
                                       std::string_view connect_str,
                                       std::string_view comment_str) {
  LakehouseTableConfig cfg;
  cfg.schema_name = std::string(schema_name);
  cfg.table_name = std::string(table_name);

  std::string_view trimmed_connect = TrimWhitespace(connect_str);
  std::string_view trimmed_comment = TrimWhitespace(comment_str);

  std::string_view uri;
  if (!trimmed_connect.empty()) {
    uri = trimmed_connect;
  } else if (!trimmed_comment.empty() && HasLakehouseScheme(trimmed_comment)) {
    uri = trimmed_comment;
  } else {
    cfg.format = LakehouseFormat::kNone;
    return cfg;
  }

  cfg.raw_uri = std::string(uri);

  std::string_view remainder;
  if (StartsWithCaseInsensitive(uri, "paimon://")) {
    cfg.format = LakehouseFormat::kPaimonPath;
    remainder = uri.substr(9);
  } else if (StartsWithCaseInsensitive(uri, "iceberg://")) {
    cfg.format = LakehouseFormat::kIcebergPath;
    remainder = uri.substr(10);
  } else if (StartsWithCaseInsensitive(uri, "iceberg_rest://")) {
    cfg.format = LakehouseFormat::kIcebergRest;
    remainder = uri.substr(15);
  } else {
    cfg.format = LakehouseFormat::kInvalid;
    cfg.error_message = "Unsupported lakehouse CONNECTION URI scheme";
    return cfg;
  }

  size_t q_pos = remainder.find('?');
  std::string_view body =
      (q_pos == std::string_view::npos) ? remainder : remainder.substr(0, q_pos);
  std::string_view query = (q_pos == std::string_view::npos)
                               ? std::string_view{}
                               : remainder.substr(q_pos + 1);

  // 1. Parse and validate query parameters first (rejecting plaintext secrets)
  size_t q_cursor = 0;
  while (!query.empty() && q_cursor <= query.size()) {
    size_t amp = query.find('&', q_cursor);
    std::string_view kv = (amp == std::string_view::npos)
                              ? query.substr(q_cursor)
                              : query.substr(q_cursor, amp - q_cursor);
    if (!kv.empty()) {
      size_t eq = kv.find('=');
      if (eq == std::string_view::npos || eq == 0) {
        cfg.format = LakehouseFormat::kInvalid;
        cfg.error_message =
            "Malformed query parameter in lakehouse CONNECTION URI";
        return cfg;
      }
      std::string_view key = TrimWhitespace(kv.substr(0, eq));
      std::string_view val = TrimWhitespace(kv.substr(eq + 1));

      if (IsForbiddenCredentialParam(key)) {
        cfg.format = LakehouseFormat::kInvalid;
        cfg.error_message =
            "Plaintext credential parameter '" + std::string(key) +
            "' is forbidden in lakehouse CONNECTION/COMMENT URI; configure "
            "DuckDB secrets or environment variables instead";
        return cfg;
      }

      std::string lower_key = ToLowerAscii(key);
      if (lower_key == "snapshot_id") {
        int64_t snap = -1;
        auto [ptr, ec] =
            std::from_chars(val.data(), val.data() + val.size(), snap);
        if (ec != std::errc{} || ptr != val.data() + val.size() || snap < 0) {
          cfg.format = LakehouseFormat::kInvalid;
          cfg.error_message =
              "Invalid snapshot_id in lakehouse URI (must be a non-negative "
              "integer)";
          return cfg;
        }
        cfg.snapshot_id = snap;
      } else if (lower_key == "version") {
        if (val.empty()) {
          cfg.format = LakehouseFormat::kInvalid;
          cfg.error_message = "Empty version parameter in lakehouse URI";
          return cfg;
        }
        cfg.iceberg_version = std::string(val);
      } else if (lower_key == "allow_moved_paths") {
        std::string lower_val = ToLowerAscii(val);
        if (lower_val == "true" || lower_val == "1") {
          cfg.allow_moved_paths = true;
        } else if (lower_val == "false" || lower_val == "0") {
          cfg.allow_moved_paths = false;
        } else {
          cfg.format = LakehouseFormat::kInvalid;
          cfg.error_message =
              "Invalid allow_moved_paths value in lakehouse URI (expected "
              "true or false)";
          return cfg;
        }
      } else if (lower_key == "endpoint" &&
                 cfg.format == LakehouseFormat::kIcebergRest) {
        if (val.empty()) {
          cfg.format = LakehouseFormat::kInvalid;
          cfg.error_message = "Empty endpoint parameter in iceberg_rest:// URI";
          return cfg;
        }
        cfg.rest_endpoint = std::string(val);
      } else if (lower_key == "warehouse" &&
                 cfg.format == LakehouseFormat::kIcebergRest) {
        if (val.empty()) {
          cfg.format = LakehouseFormat::kInvalid;
          cfg.error_message =
              "Empty warehouse parameter in iceberg_rest:// URI";
          return cfg;
        }
        cfg.rest_warehouse = std::string(val);
      } else {
        cfg.format = LakehouseFormat::kInvalid;
        cfg.error_message = "Unsupported query parameter '" +
                            std::string(key) + "' in lakehouse URI";
        return cfg;
      }
    }

    if (amp == std::string_view::npos) {
      break;
    }
    q_cursor = amp + 1;
  }

  // 2. Validate body according to format
  if (cfg.format == LakehouseFormat::kPaimonPath ||
      cfg.format == LakehouseFormat::kIcebergPath) {
    if (!ValidateTablePath(body, &cfg.table_path, &cfg.error_message)) {
      cfg.format = LakehouseFormat::kInvalid;
      return cfg;
    }
  } else if (cfg.format == LakehouseFormat::kIcebergRest) {
    if (ContainsPathTraversalSegment(body)) {
      cfg.format = LakehouseFormat::kInvalid;
      cfg.error_message =
          "Path traversal ('..') is not allowed in iceberg_rest:// URI";
      return cfg;
    }
    size_t slash_pos = body.find('/');
    if (slash_pos == std::string_view::npos || slash_pos == 0 ||
        slash_pos + 1 >= body.size()) {
      cfg.format = LakehouseFormat::kInvalid;
      cfg.error_message =
          "Invalid iceberg_rest:// URI target (expected "
          "iceberg_rest://<catalog>/<namespace>.<table>)";
      return cfg;
    }
    cfg.rest_catalog = std::string(body.substr(0, slash_pos));
    std::string_view rest_target = body.substr(slash_pos + 1);
    size_t sep_pos = rest_target.find('.');
    if (sep_pos == std::string_view::npos) {
      sep_pos = rest_target.find('/');
    }
    if (sep_pos == std::string_view::npos || sep_pos == 0 ||
        sep_pos + 1 >= rest_target.size()) {
      cfg.format = LakehouseFormat::kInvalid;
      cfg.error_message =
          "Invalid iceberg_rest:// table identifier (expected "
          "<namespace>.<table>)";
      return cfg;
    }
    cfg.rest_namespace = std::string(rest_target.substr(0, sep_pos));
    cfg.rest_table = std::string(rest_target.substr(sep_pos + 1));
  }

  return cfg;
}

std::string BuildDuckDBScanSql(const LakehouseTableConfig &config) {
  switch (config.format) {
    case LakehouseFormat::kPaimonPath: {
      std::string sql =
          "paimon_scan('" + EscapeSqlLiteral(config.table_path) + "'";
      if (config.snapshot_id >= 0) {
        sql += ", snapshot_id := " + std::to_string(config.snapshot_id);
      }
      sql += ")";
      return sql;
    }
    case LakehouseFormat::kIcebergPath: {
      std::string sql =
          "iceberg_scan('" + EscapeSqlLiteral(config.table_path) +
          "', allow_moved_paths := " +
          (config.allow_moved_paths ? "true" : "false");
      if (config.iceberg_version.has_value() &&
          !config.iceberg_version->empty()) {
        sql +=
            ", version := '" + EscapeSqlLiteral(*config.iceberg_version) + "'";
      }
      sql += ")";
      return sql;
    }
    case LakehouseFormat::kIcebergRest: {
      if (!config.table_path.empty()) {
        return "iceberg_scan('" + EscapeSqlLiteral(config.table_path) +
               "', allow_moved_paths := " +
               (config.allow_moved_paths ? "true" : "false") + ")";
      }
      return QuoteSqlIdentifier(config.rest_catalog) + "." +
             QuoteSqlIdentifier(config.rest_namespace) + "." +
             QuoteSqlIdentifier(config.rest_table);
    }
    case LakehouseFormat::kNone:
    case LakehouseFormat::kInvalid:
    default:
      return "";
  }
}

std::string TranslateLakehouseCreateView(
    const LakehouseTableConfig &config,
    const std::vector<LakehouseColumnDecl> &columns) {
  if (config.format == LakehouseFormat::kNone ||
      config.format == LakehouseFormat::kInvalid || columns.empty()) {
    return "";
  }

  std::string scan_sql = BuildDuckDBScanSql(config);
  if (scan_sql.empty()) {
    return "";
  }

  std::ostringstream ddl;
  ddl << "CREATE SCHEMA IF NOT EXISTS "
      << QuoteSqlIdentifier(config.schema_name) << ";\n";
  ddl << "CREATE OR REPLACE VIEW " << QuoteSqlIdentifier(config.schema_name)
      << "." << QuoteSqlIdentifier(config.table_name) << " (";

  for (size_t i = 0; i < columns.size(); ++i) {
    if (i > 0) {
      ddl << ", ";
    }
    ddl << QuoteSqlIdentifier(columns[i].name);
  }

  ddl << ") AS\nSELECT ";
  for (size_t i = 0; i < columns.size(); ++i) {
    if (i > 0) {
      ddl << ", ";
    }
    std::string quoted_col = QuoteSqlIdentifier(columns[i].name);
    std::string cast_type = StripCollateClause(columns[i].duckdb_type);
    ddl << "CAST(" << quoted_col << " AS " << cast_type << ") AS "
        << quoted_col;
  }

  ddl << "\nFROM " << scan_sql << ";";
  return ddl.str();
}

}  // namespace myduck::lakehouse
