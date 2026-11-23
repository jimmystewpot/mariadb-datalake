// lakehouse/test/smoke_test.cc
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include "duckdb.hpp"
#include "paimon/catalog/catalog.h"
#include "paimon/catalog/identifier.h"
#include "paimon/read_context.h"
#include "paimon/scan_context.h"
#include "paimon/schema/schema.h"
#include "paimon/table/source/table_read.h"
#include "paimon/table/source/table_scan.h"

extern "C" int32_t duckdb_register_static_extensions(void);

TEST(LakehouseFoundationSmoke, SubmoduleDepsAndCommitStampsPresent) {
  const std::filesystem::path repo_root(LAKEHOUSE_REPO_ROOT);
  const std::filesystem::path deps_prefix(LAKEHOUSE_DEPS_PREFIX);

  EXPECT_TRUE(std::filesystem::exists(repo_root / "third_parties/CRoaring/CMakeLists.txt"));
  EXPECT_TRUE(std::filesystem::exists(repo_root / "third_parties/jansson/CMakeLists.txt"));
  EXPECT_TRUE(std::filesystem::exists(repo_root / "third_parties/googletest/CMakeLists.txt"));

  const auto roaring_stamp_path = deps_prefix / ".roaring_commit";
  const auto jansson_stamp_path = deps_prefix / ".jansson_commit";
  ASSERT_TRUE(std::filesystem::exists(roaring_stamp_path));
  ASSERT_TRUE(std::filesystem::exists(jansson_stamp_path));

  std::ifstream roaring_in(roaring_stamp_path);
  std::string roaring_sha;
  std::getline(roaring_in, roaring_sha);
  EXPECT_EQ(roaring_sha.size(), 40U);

  std::ifstream jansson_in(jansson_stamp_path);
  std::string jansson_sha;
  std::getline(jansson_in, jansson_sha);
  EXPECT_EQ(jansson_sha.size(), 40U);
}

TEST(LakehouseFoundationSmoke, DependabotConfigProtectsLockstepSubmodules) {
  const std::filesystem::path repo_root(LAKEHOUSE_REPO_ROOT);
  const auto dependabot_path = repo_root / ".github/dependabot.yml";
  ASSERT_TRUE(std::filesystem::exists(dependabot_path));

  std::ifstream in(dependabot_path);
  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  EXPECT_NE(content.find("package-ecosystem: \"github-actions\""),
            std::string::npos);
  EXPECT_NE(content.find("package-ecosystem: \"gitsubmodule\""),
            std::string::npos);
  EXPECT_NE(content.find("dependency-name: \"third_parties/duckdb\""),
            std::string::npos);
  EXPECT_NE(content.find("dependency-name: \"third_parties/duckdb-quack\""),
            std::string::npos);
}

TEST(FoundationSmokeTest, StaticExtensionsIcebergAvroAndPaimonCppLinkAndExecute) {
  ASSERT_EQ(duckdb_register_static_extensions(), 0);
  duckdb::DuckDB db(nullptr);
  duckdb::Connection conn(db);

  // Verify iceberg_scan and avro_scan functions are statically registered in DuckDB
  auto ext_res = conn.Query(
      "SELECT function_name FROM duckdb_functions() "
      "WHERE function_name IN ('iceberg_scan', 'read_avro') GROUP BY function_name ORDER BY function_name");
  ASSERT_FALSE(ext_res->HasError()) << ext_res->GetError();
  auto chunk = ext_res->Fetch();
  ASSERT_NE(chunk, nullptr);
  EXPECT_EQ(chunk->size(), 2u);

  // Verify real apache/paimon-cpp reads golden parquet table append_09.db/append_09
  std::string warehouse = std::string(PAIMON_GOLDEN_DATA_DIR) + "/parquet";
  std::string table_path = warehouse + "/append_09.db/append_09";

  auto cat_res = paimon::Catalog::Create(warehouse, {});
  ASSERT_TRUE(cat_res.ok()) << cat_res.status().ToString();
  auto schema_res = cat_res.value()->LoadTableSchema(paimon::Identifier("append_09", "append_09"));
  ASSERT_TRUE(schema_res.ok()) << schema_res.status().ToString();
  EXPECT_FALSE(schema_res.value()->FieldNames().empty());

  paimon::ScanContextBuilder scan_builder(table_path);
  auto scan_ctx = scan_builder.Finish();
  ASSERT_TRUE(scan_ctx.ok()) << scan_ctx.status().ToString();
  auto scan = paimon::TableScan::Create(std::move(scan_ctx).value());
  ASSERT_TRUE(scan.ok()) << scan.status().ToString();
  auto plan = scan.value()->CreatePlan();
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  EXPECT_FALSE(plan.value()->Splits().empty());
}
