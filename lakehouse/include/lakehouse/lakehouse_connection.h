#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace myduck::lakehouse {

enum class LakehouseFormat {
  kNone,
  kIcebergPath,
  kIcebergRest,
  kPaimonPath,
  kInvalid
};

struct LakehouseColumnDecl {
  std::string name;
  std::string duckdb_type;
};

struct LakehouseTableConfig {
  LakehouseFormat format = LakehouseFormat::kNone;
  std::string schema_name;
  std::string table_name;
  std::string raw_uri;
  std::string table_path;
  std::string rest_catalog;
  std::string rest_namespace;
  std::string rest_table;
  std::string rest_endpoint;
  std::string rest_warehouse;
  int64_t snapshot_id = -1;
  std::optional<std::string> iceberg_version;
  bool allow_moved_paths = true;
  std::string error_message;
};

void SetAllowedPathPrefix(std::string prefix);

LakehouseTableConfig ParseLakehouseUri(std::string_view schema_name,
                                       std::string_view table_name,
                                       std::string_view connect_str,
                                       std::string_view comment_str = "");

std::string BuildDuckDBScanSql(const LakehouseTableConfig &config);

std::string TranslateLakehouseCreateView(
    const LakehouseTableConfig &config,
    const std::vector<LakehouseColumnDecl> &columns);

}  // namespace myduck::lakehouse
