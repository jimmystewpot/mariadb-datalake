#include "lakehouse/paimon_arrow_stream.h"

#include <cerrno>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "paimon/catalog/catalog.h"
#include "paimon/catalog/identifier.h"
#include "paimon/read_context.h"
#include "paimon/reader/batch_reader.h"
#include "paimon/status.h"
#include "paimon/table/source/table_read.h"

namespace myduck::lakehouse {
namespace {

struct PaimonBatchHolder {
  std::unique_ptr<ArrowArray> raw_array;
  std::unique_ptr<ArrowSchema> raw_schema;

  PaimonBatchHolder(std::unique_ptr<ArrowArray> arr, std::unique_ptr<ArrowSchema> sch)
      : raw_array(std::move(arr)), raw_schema(std::move(sch)) {}

  ~PaimonBatchHolder() {
    if (raw_array && raw_array->release) {
      raw_array->release(raw_array.get());
    }
    if (raw_schema && raw_schema->release) {
      raw_schema->release(raw_schema.get());
    }
  }
};

void ReleasePaimonBatchHolder(ArrowArray *array) {
  if (!array || !array->release) {
    return;
  }
  auto *holder = static_cast<PaimonBatchHolder *>(array->private_data);
  delete holder;
  array->private_data = nullptr;
  array->release = nullptr;
}

struct PaimonStreamState {
  std::string table_path;
  std::vector<std::string> projected_field_names;
  std::unique_ptr<paimon::TableRead> table_read;
  std::unique_ptr<paimon::BatchReader> batch_reader;
  std::string last_error;
  std::string init_error;
  bool eof = false;
};

int StreamGetSchema(ArrowArrayStream *stream, ArrowSchema *out) {
  if (!stream || !stream->private_data || !out) {
    return EINVAL;
  }
  auto *state = static_cast<PaimonStreamState *>(stream->private_data);
  auto schema_res = LoadPaimonSchemaFromPath(state->table_path);
  if (!schema_res.ok()) {
    state->last_error = schema_res.status().ToString();
    return EIO;
  }
  auto arrow_res = schema_res.value()->GetArrowSchema();
  if (!arrow_res.ok()) {
    state->last_error = arrow_res.status().ToString();
    return EIO;
  }
  *out = *arrow_res.value();
  arrow_res.value()->release = nullptr;
  return 0;
}

int StreamGetNext(ArrowArrayStream *stream, ArrowArray *out) {
  if (!stream || !stream->private_data || !out) {
    return EINVAL;
  }
  auto *state = static_cast<PaimonStreamState *>(stream->private_data);
  out->Init();

  if (!state->init_error.empty()) {
    state->last_error = state->init_error;
    return EIO;
  }
  if (state->eof || !state->batch_reader) {
    out->release = nullptr;
    return 0;
  }

  auto batch_res = state->batch_reader->NextBatch();
  if (!batch_res.ok()) {
    state->last_error = batch_res.status().ToString();
    return EIO;
  }

  auto batch = std::move(batch_res).value();
  if (paimon::BatchReader::IsEofBatch(batch) || !batch.first || !batch.first->release) {
    state->eof = true;
    out->release = nullptr;
    return 0;
  }

  ArrowArray *raw = batch.first.get();
  if (raw->n_children < 1) {
    state->last_error = "Paimon BatchReader returned ArrowArray with n_children < 1";
    return EIO;
  }

  // Strip child 0 (_VALUE_KIND) while retaining the root ArrowArray and ArrowSchema
  // as one complete Arrow C Data Interface ownership unit per batch_reader.h:50-53.
  out->length = raw->length;
  out->null_count = 0;
  out->offset = raw->offset;
  out->n_buffers = raw->n_buffers;
  out->buffers = raw->buffers;
  out->n_children = raw->n_children - 1;
  out->children = (raw->n_children > 1) ? (raw->children + 1) : nullptr;
  out->dictionary = nullptr;
  out->private_data = new PaimonBatchHolder(std::move(batch.first), std::move(batch.second));
  out->release = ReleasePaimonBatchHolder;
  return 0;
}

const char *StreamGetLastError(ArrowArrayStream *stream) {
  if (!stream || !stream->private_data) {
    return "Invalid Paimon ArrowArrayStream";
  }
  auto *state = static_cast<PaimonStreamState *>(stream->private_data);
  return state->last_error.c_str();
}

void StreamRelease(ArrowArrayStream *stream) {
  if (!stream || !stream->release) {
    return;
  }
  auto *state = static_cast<PaimonStreamState *>(stream->private_data);
  if (state) {
    if (state->batch_reader) {
      state->batch_reader->Close();
      state->batch_reader.reset();
    }
    state->table_read.reset();
    delete state;
  }
  stream->private_data = nullptr;
  stream->release = nullptr;
}

}  // namespace

PaimonPathComponents DecomposePaimonTablePath(std::string_view table_path) {
  while (table_path.size() > 1 && table_path.back() == '/') {
    table_path.remove_suffix(1);
  }
  PaimonPathComponents result;
  std::filesystem::path p(table_path);
  result.table_name = p.filename().string();
  std::filesystem::path parent = p.parent_path();
  std::string parent_name = parent.filename().string();
  std::filesystem::path grandparent = parent.parent_path();

  constexpr std::string_view kDbSuffix = ".db";
  if (parent_name.size() > kDbSuffix.size() &&
      std::string_view(parent_name).substr(parent_name.size() - kDbSuffix.size()) == kDbSuffix &&
      !grandparent.empty() && !result.table_name.empty()) {
    result.warehouse_root = grandparent.string();
    result.db_name = parent_name.substr(0, parent_name.size() - kDbSuffix.size());
    result.is_catalog_layout = true;
  } else {
    result.warehouse_root = parent.string();
    result.db_name = parent_name;
    result.is_catalog_layout = false;
  }
  return result;
}

paimon::Result<std::shared_ptr<paimon::Schema>> LoadPaimonSchemaFromPath(
    const std::string &table_path) {
  auto comp = DecomposePaimonTablePath(table_path);
  if (comp.table_name.empty() || comp.warehouse_root.empty()) {
    return paimon::Status::Invalid("Invalid Paimon table path: " + table_path);
  }

  if (comp.is_catalog_layout) {
    PAIMON_ASSIGN_OR_RAISE(auto catalog, paimon::Catalog::Create(comp.warehouse_root, {}));
    return catalog->LoadTableSchema(paimon::Identifier(comp.db_name, comp.table_name));
  }

  std::error_code ec;
  if (!std::filesystem::exists(table_path, ec)) {
    return paimon::Status::IOError("Paimon table path does not exist: " + table_path);
  }
  auto temp_wh = std::filesystem::temp_directory_path(ec) /
                 ("paimon_standalone_cat_" + comp.table_name);
  std::filesystem::remove_all(temp_wh, ec);
  std::filesystem::create_directories(temp_wh / "default.db", ec);
  std::filesystem::create_directory_symlink(table_path, temp_wh / "default.db" / comp.table_name,
                                            ec);
  if (ec) {
    std::filesystem::remove_all(temp_wh, ec);
    return paimon::Status::IOError("Failed to link standalone Paimon table path: " + table_path);
  }
  auto cat_res = paimon::Catalog::Create(temp_wh.string(), {});
  if (!cat_res.ok()) {
    std::filesystem::remove_all(temp_wh, ec);
    return cat_res.status();
  }
  auto schema_res =
      cat_res.value()->LoadTableSchema(paimon::Identifier("default", comp.table_name));
  std::filesystem::remove_all(temp_wh, ec);
  return schema_res;
}

std::unique_ptr<duckdb::ArrowArrayStreamWrapper> CreatePaimonArrowStream(
    const std::string &table_path,
    const std::vector<std::shared_ptr<paimon::Split>> &splits,
    const std::vector<std::string> &projected_field_names) {
  auto wrapper = std::make_unique<duckdb::ArrowArrayStreamWrapper>();
  auto *state = new PaimonStreamState();
  state->table_path = table_path;
  state->projected_field_names = projected_field_names;

  if (splits.empty()) {
    state->eof = true;
  } else {
    paimon::ReadContextBuilder read_builder(table_path);
    if (!projected_field_names.empty()) {
      read_builder.SetReadFieldNames(projected_field_names);
    }
    auto read_ctx_res = read_builder.Finish();
    if (!read_ctx_res.ok()) {
      state->init_error = read_ctx_res.status().ToString();
    } else {
      auto table_read_res = paimon::TableRead::Create(std::move(read_ctx_res).value());
      if (!table_read_res.ok()) {
        state->init_error = table_read_res.status().ToString();
      } else {
        state->table_read = std::move(table_read_res).value();
        auto reader_res = state->table_read->CreateReader(splits);
        if (!reader_res.ok()) {
          state->init_error = reader_res.status().ToString();
        } else {
          state->batch_reader = std::move(reader_res).value();
        }
      }
    }
  }

  wrapper->arrow_array_stream.get_schema = StreamGetSchema;
  wrapper->arrow_array_stream.get_next = StreamGetNext;
  wrapper->arrow_array_stream.get_last_error = StreamGetLastError;
  wrapper->arrow_array_stream.release = StreamRelease;
  wrapper->arrow_array_stream.private_data = state;
  return wrapper;
}

}  // namespace myduck::lakehouse
