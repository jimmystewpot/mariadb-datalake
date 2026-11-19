#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "duckdb/common/arrow/arrow_wrapper.hpp"
#include "paimon/result.h"
#include "paimon/schema/schema.h"
#include "paimon/table/source/split.h"

namespace myduck::lakehouse {

struct PaimonPathComponents {
  std::string warehouse_root;
  std::string db_name;
  std::string table_name;
  bool is_catalog_layout = false;
};

PaimonPathComponents DecomposePaimonTablePath(std::string_view table_path);

paimon::Result<std::shared_ptr<paimon::Schema>> LoadPaimonSchemaFromPath(
    const std::string &table_path);

std::unique_ptr<duckdb::ArrowArrayStreamWrapper> CreatePaimonArrowStream(
    const std::string &table_path,
    const std::vector<std::shared_ptr<paimon::Split>> &splits,
    const std::vector<std::string> &projected_field_names);

}  // namespace myduck::lakehouse
