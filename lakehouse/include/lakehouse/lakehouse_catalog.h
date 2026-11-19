#pragma once

#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "lakehouse/lakehouse_connection.h"

namespace duckdb {
class Connection;
}  // namespace duckdb

namespace myduck::lakehouse {

class LakehouseCatalog {
 public:
  static LakehouseCatalog &Instance();

  void RegisterTable(const LakehouseTableConfig &config);
  void UnregisterTable(std::string_view schema_name, std::string_view table_name);
  void UnregisterSchema(std::string_view schema_name);
  void RenameTable(std::string_view old_schema, std::string_view old_table,
                   std::string_view new_schema, std::string_view new_table);
  bool IsLakehouseTable(std::string_view schema_name,
                        std::string_view table_name) const;
  std::optional<LakehouseTableConfig> FindTable(
      std::string_view schema_name, std::string_view table_name) const;

  bool AttachOrResolveIcebergRest(duckdb::Connection &conn,
                                  const LakehouseTableConfig &config,
                                  std::string *resolved_scan_sql,
                                  std::string *error_out);

  void ClearForTesting();

 private:
  LakehouseCatalog() = default;

  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, LakehouseTableConfig> tables_;
};

bool IsDuckDBView(duckdb::Connection &conn, std::string_view schema_name,
                  std::string_view table_name);

bool ExecuteLakehouseCreateTable(
    duckdb::Connection &conn, const LakehouseTableConfig &cfg,
    const std::vector<LakehouseColumnDecl> &cols,
    std::string *error_out = nullptr);

bool ExecuteLakehouseDropTable(duckdb::Connection &conn,
                               std::string_view schema_name,
                               std::string_view table_name,
                               std::string *error_out = nullptr);

}  // namespace myduck::lakehouse
