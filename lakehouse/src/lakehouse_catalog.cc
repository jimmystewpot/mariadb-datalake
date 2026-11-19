#include "lakehouse/lakehouse_catalog.h"

namespace myduck::lakehouse {

LakehouseCatalog &LakehouseCatalog::Instance() {
  static LakehouseCatalog instance;
  return instance;
}

void LakehouseCatalog::RegisterTable(const LakehouseTableConfig &config) {
  (void)config;
}

void LakehouseCatalog::UnregisterTable(std::string_view schema_name,
                                       std::string_view table_name) {
  (void)schema_name;
  (void)table_name;
}

void LakehouseCatalog::RenameTable(std::string_view old_schema,
                                   std::string_view old_table,
                                   std::string_view new_schema,
                                   std::string_view new_table) {
  (void)old_schema;
  (void)old_table;
  (void)new_schema;
  (void)new_table;
}

bool LakehouseCatalog::IsLakehouseTable(std::string_view schema_name,
                                        std::string_view table_name) const {
  (void)schema_name;
  (void)table_name;
  return false;
}

std::optional<LakehouseTableConfig> LakehouseCatalog::FindTable(
    std::string_view schema_name, std::string_view table_name) const {
  (void)schema_name;
  (void)table_name;
  return std::nullopt;
}

bool LakehouseCatalog::AttachOrResolveIcebergRest(
    duckdb::Connection &conn, const LakehouseTableConfig &config,
    std::string *resolved_scan_sql, std::string *error_out) {
  (void)conn;
  (void)config;
  (void)resolved_scan_sql;
  if (error_out) {
    *error_out = "Not implemented";
  }
  return false;
}

void LakehouseCatalog::ClearForTesting() {}

}  // namespace myduck::lakehouse
