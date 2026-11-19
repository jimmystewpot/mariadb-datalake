#pragma once

namespace duckdb {
class DatabaseInstance;
}  // namespace duckdb

namespace myduck::lakehouse {

void RegisterPaimonScan(duckdb::DatabaseInstance &db);

}  // namespace myduck::lakehouse
