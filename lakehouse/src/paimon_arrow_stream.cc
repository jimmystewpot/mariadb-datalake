#include "lakehouse/paimon_arrow_stream.h"
#include "paimon/status.h"

namespace myduck::lakehouse {

PaimonPathComponents DecomposePaimonTablePath(std::string_view table_path) {
  (void)table_path;
  return PaimonPathComponents{};
}

paimon::Result<std::shared_ptr<paimon::Schema>> LoadPaimonSchemaFromPath(
    const std::string &table_path) {
  (void)table_path;
  return paimon::Status::NotImplemented("LoadPaimonSchemaFromPath not implemented");
}

std::unique_ptr<duckdb::ArrowArrayStreamWrapper> CreatePaimonArrowStream(
    const std::string &table_path,
    const std::vector<std::shared_ptr<paimon::Split>> &splits,
    const std::vector<std::string> &projected_field_names) {
  (void)table_path;
  (void)splits;
  (void)projected_field_names;
  return nullptr;
}

}  // namespace myduck::lakehouse
