#include "lakehouse/lakehouse_connection.h"

namespace myduck::lakehouse {

void SetAllowedPathPrefix(std::string prefix) { (void)prefix; }

LakehouseTableConfig ParseLakehouseUri(std::string_view schema_name,
                                       std::string_view table_name,
                                       std::string_view connect_str,
                                       std::string_view comment_str) {
  (void)schema_name;
  (void)table_name;
  (void)connect_str;
  (void)comment_str;
  return LakehouseTableConfig{};
}

std::string BuildDuckDBScanSql(const LakehouseTableConfig &config) {
  (void)config;
  return "";
}

std::string TranslateLakehouseCreateView(
    const LakehouseTableConfig &config,
    const std::vector<LakehouseColumnDecl> &columns) {
  (void)config;
  (void)columns;
  return "";
}

}  // namespace myduck::lakehouse
