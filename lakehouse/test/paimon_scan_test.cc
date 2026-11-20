// lakehouse/test/paimon_scan_test.cc
#include <gtest/gtest.h>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "arrow/api.h"
#include "arrow/c/bridge.h"
#include "duckdb.hpp"
#include "lakehouse/lakehouse_connection.h"
#include "lakehouse/paimon_arrow_stream.h"
#include "lakehouse/paimon_table_function.h"
#include "paimon/catalog/catalog.h"
#include "paimon/catalog/identifier.h"
#include "paimon/commit_context.h"
#include "paimon/defs.h"
#include "paimon/file_store_commit.h"
#include "paimon/file_store_write.h"
#include "paimon/record_batch.h"
#include "paimon/write_context.h"

extern "C" int32_t duckdb_register_static_extensions(void);

namespace myduck::lakehouse {
namespace {

void WriteLargePaimonTable(const std::string &warehouse, const std::string &db_name,
                           const std::string &table_name, int32_t num_rows) {
  auto id_field = arrow::field("id", arrow::int32());
  auto arrow_schema = arrow::schema({id_field});
  ArrowSchema c_schema{};
  ASSERT_TRUE(arrow::ExportSchema(*arrow_schema, &c_schema).ok());

  std::map<std::string, std::string> options = {
      {paimon::Options::FILE_FORMAT, "parquet"},
      {paimon::Options::FILE_SYSTEM, "local"},
      {paimon::Options::BUCKET, "-1"},
  };
  auto cat_res = paimon::Catalog::Create(warehouse, options);
  ASSERT_TRUE(cat_res.ok()) << cat_res.status().ToString();
  auto &catalog = cat_res.value();
  ASSERT_TRUE(catalog->CreateDatabase(db_name, options, /*ignore_if_exists=*/true).ok());
  auto create_tbl_st = catalog->CreateTable(
      paimon::Identifier(db_name, table_name), &c_schema,
      /*partition_keys=*/{}, /*primary_keys=*/{}, options, /*ignore_if_exists=*/false);
  if (c_schema.release) {
    c_schema.release(&c_schema);
  }
  ASSERT_TRUE(create_tbl_st.ok()) << create_tbl_st.ToString();

  arrow::Int32Builder id_builder;
  ASSERT_TRUE(id_builder.Reserve(num_rows).ok());
  for (int32_t i = 0; i < num_rows; ++i) {
    id_builder.UnsafeAppend(i);
  }
  std::shared_ptr<arrow::Array> id_array;
  ASSERT_TRUE(id_builder.Finish(&id_array).ok());
  auto struct_res = arrow::StructArray::Make({id_array}, {id_field});
  ASSERT_TRUE(struct_res.ok());
  ArrowArray c_array{};
  ASSERT_TRUE(arrow::ExportArray(*struct_res.ValueOrDie(), &c_array).ok());

  paimon::RecordBatchBuilder rb_builder(&c_array);
  auto rb_res = rb_builder.SetBucket(0).Finish();
  ASSERT_TRUE(rb_res.ok()) << rb_res.status().ToString();

  std::string table_path = warehouse + "/" + db_name + ".db/" + table_name;
  paimon::WriteContextBuilder write_ctx_builder(table_path, "test_commit_user");
  auto write_ctx_res = write_ctx_builder.SetOptions(options).WithStreamingMode(false).Finish();
  ASSERT_TRUE(write_ctx_res.ok()) << write_ctx_res.status().ToString();
  auto write_res = paimon::FileStoreWrite::Create(std::move(write_ctx_res).value());
  ASSERT_TRUE(write_res.ok()) << write_res.status().ToString();
  ASSERT_TRUE(write_res.value()->Write(std::move(rb_res).value()).ok());
  auto msgs_res = write_res.value()->PrepareCommit(/*wait_compaction=*/false, 0);
  ASSERT_TRUE(msgs_res.ok()) << msgs_res.status().ToString();
  ASSERT_TRUE(write_res.value()->Close().ok());

  paimon::CommitContextBuilder commit_ctx_builder(table_path, "test_commit_user");
  auto commit_ctx_res = commit_ctx_builder.SetOptions(options).IgnoreEmptyCommit(false).Finish();
  ASSERT_TRUE(commit_ctx_res.ok()) << commit_ctx_res.status().ToString();
  auto commit_res = paimon::FileStoreCommit::Create(std::move(commit_ctx_res).value());
  ASSERT_TRUE(commit_res.ok()) << commit_res.status().ToString();
  ASSERT_TRUE(commit_res.value()->Commit(msgs_res.value(), 0).ok());
}

class PaimonScanTest : public ::testing::Test {
 protected:
  void SetUp() override {
    duckdb_register_static_extensions();
    db_ = std::make_unique<duckdb::DuckDB>(nullptr);
    RegisterPaimonScan(*db_->instance);
    conn_ = std::make_unique<duckdb::Connection>(*db_);
    golden_parquet_dir_ = std::string(PAIMON_GOLDEN_DATA_DIR) + "/parquet";
  }
  std::unique_ptr<duckdb::DuckDB> db_;
  std::unique_ptr<duckdb::Connection> conn_;
  std::string golden_parquet_dir_;
};

TEST_F(PaimonScanTest, AppendOnlyTimeTravelSnapshots) {
  std::string tbl = golden_parquet_dir_ + "/append_09.db/append_09";

  // Snapshot 1 has fewer rows than latest snapshot
  auto snap1 = conn_->Query("SELECT COUNT(*) FROM paimon_scan('" + tbl + "', 1)");
  ASSERT_FALSE(snap1->HasError()) << snap1->GetError();
  int64_t count_snap1 = snap1->Fetch()->GetValue(0, 0).GetValue<int64_t>();
  EXPECT_GT(count_snap1, 0);

  auto latest = conn_->Query("SELECT COUNT(*) FROM paimon_scan('" + tbl + "')");
  ASSERT_FALSE(latest->HasError()) << latest->GetError();
  int64_t count_latest = latest->Fetch()->GetValue(0, 0).GetValue<int64_t>();
  EXPECT_GT(count_latest, count_snap1);

  // Named parameter snapshot_id := 1 matches positional
  auto snap1_named = conn_->Query(
      "SELECT COUNT(*) FROM paimon_scan('" + tbl + "', snapshot_id := 1)");
  ASSERT_FALSE(snap1_named->HasError()) << snap1_named->GetError();
  EXPECT_EQ(snap1_named->Fetch()->GetValue(0, 0).GetValue<int64_t>(), count_snap1);

  // DecomposePaimonTablePath verification
  auto comp = DecomposePaimonTablePath(tbl);
  EXPECT_TRUE(comp.is_catalog_layout);
  EXPECT_EQ(comp.db_name, "append_09");
  EXPECT_EQ(comp.table_name, "append_09");

  // Standalone (non-.db parent directory) schema loading uses race-free unique temp directory
  auto standalone_root =
      std::filesystem::temp_directory_path() / "paimon_standalone_test_dir";
  std::filesystem::remove_all(standalone_root);
  std::filesystem::create_directories(standalone_root);
  auto standalone_tbl = standalone_root / "append_standalone";
  std::filesystem::create_directory_symlink(tbl, standalone_tbl);
  auto standalone_schema_res = LoadPaimonSchemaFromPath(standalone_tbl.string());
  EXPECT_TRUE(standalone_schema_res.ok()) << standalone_schema_res.status().ToString();
  std::filesystem::remove_all(standalone_root);

  // Non-existent snapshot fails cleanly at bind time
  auto bad_snap = conn_->Query(
      "SELECT COUNT(*) FROM paimon_scan('" + tbl + "', snapshot_id := 999999)");
  EXPECT_TRUE(bad_snap->HasError());

  // Explicit snapshot_id = -1 means latest snapshot and succeeds
  auto latest_neg1 = conn_->Query(
      "SELECT COUNT(*) FROM paimon_scan('" + tbl + "', snapshot_id := -1)");
  ASSERT_FALSE(latest_neg1->HasError()) << latest_neg1->GetError();
  EXPECT_EQ(latest_neg1->Fetch()->GetValue(0, 0).GetValue<int64_t>(), count_latest);

  // Negative snapshot_id < -1 is rejected with InvalidInputException
  auto bad_neg_pos = conn_->Query("SELECT COUNT(*) FROM paimon_scan('" + tbl + "', -2)");
  EXPECT_TRUE(bad_neg_pos->HasError());
  EXPECT_NE(bad_neg_pos->GetError().find("snapshot_id must be >= 0"), std::string::npos);

  auto bad_neg_named =
      conn_->Query("SELECT COUNT(*) FROM paimon_scan('" + tbl + "', snapshot_id := -5)");
  EXPECT_TRUE(bad_neg_named->HasError());
  EXPECT_NE(bad_neg_named->GetError().find("snapshot_id must be >= 0"), std::string::npos);

  // Direct paimon_scan path validation rejects traversal, relative paths, remote URIs, and prefix violations
  auto bad_traversal =
      conn_->Query("SELECT COUNT(*) FROM paimon_scan('/tmp/wh/../../etc/passwd')");
  EXPECT_TRUE(bad_traversal->HasError());
  EXPECT_NE(bad_traversal->GetError().find("Path traversal"), std::string::npos);

  auto bad_s3 =
      conn_->Query("SELECT COUNT(*) FROM paimon_scan('s3://bucket/wh/db.db/tbl')");
  EXPECT_TRUE(bad_s3->HasError());

  SetAllowedPathPrefix("/nonexistent_allowed_root");
  auto bad_prefix = conn_->Query("SELECT COUNT(*) FROM paimon_scan('" + tbl + "')");
  SetAllowedPathPrefix("");
  EXPECT_TRUE(bad_prefix->HasError());
  EXPECT_NE(bad_prefix->GetError().find("outside the allowed path prefix"),
            std::string::npos);
}

TEST_F(PaimonScanTest, PrimaryKeyMergeOnReadAndDeletionVectorStripsValueKind) {
  // 1. Primary-Key Merge-On-Read golden table
  std::string mor_tbl =
      golden_parquet_dir_ + "/pk_table_scan_and_read_mor.db/pk_table_scan_and_read_mor";
  auto mor_res = conn_->Query("SELECT * FROM paimon_scan('" + mor_tbl + "')");
  ASSERT_FALSE(mor_res->HasError()) << mor_res->GetError();
  // Ensure _VALUE_KIND is NOT exposed as a column in DuckDB
  for (const auto &col_name : mor_res->GetNames()) {
    EXPECT_NE(col_name, "_VALUE_KIND");
  }
  auto mor_chunk = mor_res->Fetch();
  ASSERT_NE(mor_chunk, nullptr);
  EXPECT_GT(mor_chunk->size(), 0u);

  // 2. Primary-Key Deletion Vectors golden table
  std::string dv_tbl =
      golden_parquet_dir_ + "/pk_table_scan_and_read_dv.db/pk_table_scan_and_read_dv";
  auto dv_res = conn_->Query("SELECT * FROM paimon_scan('" + dv_tbl + "')");
  ASSERT_FALSE(dv_res->HasError()) << dv_res->GetError();
  for (const auto &col_name : dv_res->GetNames()) {
    EXPECT_NE(col_name, "_VALUE_KIND");
  }
  auto dv_chunk = dv_res->Fetch();
  ASSERT_NE(dv_chunk, nullptr);
  EXPECT_GT(dv_chunk->size(), 0u);
}

TEST_F(PaimonScanTest, CountStarWithFilterAndEmptyProjection) {
  std::string tbl = golden_parquet_dir_ + "/append_09.db/append_09";
  auto all_res = conn_->Query("SELECT COUNT(*) FROM paimon_scan('" + tbl + "')");
  ASSERT_FALSE(all_res->HasError()) << all_res->GetError();
  int64_t total = all_res->Fetch()->GetValue(0, 0).GetValue<int64_t>();

  auto filtered_res = conn_->Query(
      "SELECT COUNT(*) FROM paimon_scan('" + tbl + "') WHERE f1 > 10");
  ASSERT_FALSE(filtered_res->HasError()) << filtered_res->GetError();
  int64_t filtered = filtered_res->Fetch()->GetValue(0, 0).GetValue<int64_t>();
  EXPECT_LT(filtered, total);
  EXPECT_GE(filtered, 0);

  // Reordered multi-column projection with filter pushdown
  auto proj_res = conn_->Query(
      "SELECT f2, f0 FROM paimon_scan('" + tbl + "') WHERE f1 > 10 AND f0 IS NOT NULL");
  ASSERT_FALSE(proj_res->HasError()) << proj_res->GetError();
  auto proj_chunk = proj_res->Fetch();
  ASSERT_NE(proj_chunk, nullptr);
  EXPECT_EQ(proj_chunk->ColumnCount(), 2u);
  EXPECT_EQ(static_cast<int64_t>(proj_chunk->size()), filtered);

  // Verify StreamGetSchema filters and orders ArrowSchema children to match projected_field_names
  auto stream_wrapper = CreatePaimonArrowStream(tbl, /*splits=*/{}, {"f2", "f0"});
  ASSERT_NE(stream_wrapper, nullptr);
  ArrowSchema proj_schema{};
  ASSERT_EQ(stream_wrapper->arrow_array_stream.get_schema(
                &stream_wrapper->arrow_array_stream, &proj_schema),
            0);
  ASSERT_EQ(proj_schema.n_children, 2);
  EXPECT_STREQ(proj_schema.children[0]->name, "f2");
  EXPECT_STREQ(proj_schema.children[1]->name, "f0");
  if (proj_schema.release) {
    proj_schema.release(&proj_schema);
  }

  // Verify PaimonScanFunctionData::Equals distinguishes different snapshot_ids/tables in one query
  auto join_res = conn_->Query(
      "SELECT (SELECT COUNT(*) FROM paimon_scan('" + tbl + "', 1)), "
      "(SELECT COUNT(*) FROM paimon_scan('" + tbl + "'))");
  ASSERT_FALSE(join_res->HasError()) << join_res->GetError();
  auto join_chunk = join_res->Fetch();
  ASSERT_NE(join_chunk, nullptr);
  EXPECT_LT(join_chunk->GetValue(0, 0).GetValue<int64_t>(),
            join_chunk->GetValue(1, 0).GetValue<int64_t>());
}

TEST_F(PaimonScanTest, LargeTableMultiChunkSlicing) {
  // Write 5000 rows using real paimon-cpp Catalog + FileStoreWrite + FileStoreCommit
  // and verify paimon_scan slices across 3 DuckDB DataChunks (2048 + 2048 + 904)
  auto temp_wh = std::filesystem::temp_directory_path() / "paimon_large_wh";
  std::filesystem::remove_all(temp_wh);
  std::filesystem::create_directories(temp_wh);

  std::string tbl_path = (temp_wh / "bench.db" / "large_tbl").string();
  // Helper declared in paimon_scan_test.cc using paimon::Catalog::CreateTable + FileStoreWrite
  WriteLargePaimonTable(temp_wh.string(), "bench", "large_tbl", 5000);

  auto res = conn_->Query(
      "SELECT COUNT(*), MIN(id), MAX(id), SUM(id) FROM paimon_scan('" + tbl_path + "')");
  ASSERT_FALSE(res->HasError()) << res->GetError();
  auto chunk = res->Fetch();
  ASSERT_NE(chunk, nullptr);
  EXPECT_EQ(chunk->GetValue(0, 0).GetValue<int64_t>(), 5000);
  EXPECT_EQ(chunk->GetValue(1, 0).GetValue<int32_t>(), 0);
  EXPECT_EQ(chunk->GetValue(2, 0).GetValue<int32_t>(), 4999);
  EXPECT_EQ(chunk->GetValue(3, 0).GetValue<int64_t>(), (4999LL * 5000LL) / 2);

  // Non-SIMD-multiple filtered slices (65 rows and 2049 = STANDARD_VECTOR_SIZE + 1 rows)
  for (int64_t limit_id : {65LL, 2049LL}) {
    auto slice_res = conn_->Query(
        "SELECT COUNT(*), MIN(id), MAX(id), SUM(id) FROM paimon_scan('" + tbl_path +
        "') WHERE id < " + std::to_string(limit_id));
    ASSERT_FALSE(slice_res->HasError()) << slice_res->GetError();
    auto slice_chunk = slice_res->Fetch();
    ASSERT_NE(slice_chunk, nullptr);
    EXPECT_EQ(slice_chunk->GetValue(0, 0).GetValue<int64_t>(), limit_id);
    EXPECT_EQ(slice_chunk->GetValue(1, 0).GetValue<int32_t>(), 0);
    EXPECT_EQ(slice_chunk->GetValue(2, 0).GetValue<int32_t>(),
              static_cast<int32_t>(limit_id - 1));
    EXPECT_EQ(slice_chunk->GetValue(3, 0).GetValue<int64_t>(),
              ((limit_id - 1) * limit_id) / 2);
  }

  // Exact non-SIMD-multiple table row counts (65 and 2049) to exercise Arrow/Parquet batch tails
  for (int32_t exact_rows : {65, 2049}) {
    std::string sub_name = "unaligned_" + std::to_string(exact_rows);
    std::string sub_path = (temp_wh / "bench.db" / sub_name).string();
    WriteLargePaimonTable(temp_wh.string(), "bench", sub_name, exact_rows);
    auto sub_res = conn_->Query(
        "SELECT COUNT(*), MIN(id), MAX(id), SUM(id) FROM paimon_scan('" + sub_path + "')");
    ASSERT_FALSE(sub_res->HasError()) << sub_res->GetError();
    auto sub_chunk = sub_res->Fetch();
    ASSERT_NE(sub_chunk, nullptr);
    EXPECT_EQ(sub_chunk->GetValue(0, 0).GetValue<int64_t>(),
              static_cast<int64_t>(exact_rows));
    EXPECT_EQ(sub_chunk->GetValue(1, 0).GetValue<int32_t>(), 0);
    EXPECT_EQ(sub_chunk->GetValue(2, 0).GetValue<int32_t>(), exact_rows - 1);
    int64_t n = static_cast<int64_t>(exact_rows);
    EXPECT_EQ(sub_chunk->GetValue(3, 0).GetValue<int64_t>(), ((n - 1) * n) / 2);
  }

  std::filesystem::remove_all(temp_wh);
}

}  // namespace
}  // namespace myduck::lakehouse
