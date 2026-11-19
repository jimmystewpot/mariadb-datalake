// lakehouse/test/lakehouse_e2e_test.cc
#include <gtest/gtest.h>
#include <thread>
#include "duckdb.hpp"
#include "httplib.hpp"
#include "lakehouse/lakehouse_catalog.h"
#include "lakehouse/lakehouse_connection.h"
#include "lakehouse/paimon_table_function.h"
#include "mariadb_dialect.h"

extern "C" int32_t duckdb_register_static_extensions(void);

namespace myduck::lakehouse {
namespace {

class LakehouseE2ETest : public ::testing::Test {
 protected:
  void SetUp() override {
    duckdb_register_static_extensions();
    LakehouseCatalog::Instance().ClearForTesting();
    db_ = std::make_unique<duckdb::DuckDB>(nullptr);
    RegisterPaimonScan(*db_->instance);
    myduck::register_mariadb_dialect(*db_->instance);
    conn_ = std::make_unique<duckdb::Connection>(*db_);
    // Mirror DuckdbManager::Initialize() lockdown after registering paimon_scan
    ASSERT_FALSE(conn_->Query("SET autoinstall_known_extensions=false")->HasError());
    ASSERT_FALSE(conn_->Query("SET allow_community_extensions=false")->HasError());
    ASSERT_FALSE(conn_->Query("SET lock_configuration=true")->HasError());
  }
  std::unique_ptr<duckdb::DuckDB> db_;
  std::unique_ptr<duckdb::Connection> conn_;
};

TEST_F(LakehouseE2ETest, ViewDdlRejectsMissingTableOrMismatchedColumn) {
  auto missing_cfg = ParseLakehouseUri("lake", "bad_tbl", "paimon:///nonexistent/wh/db.db/tbl");
  std::vector<LakehouseColumnDecl> cols = {{"id", "INTEGER"}};
  auto res1 = conn_->Query(TranslateLakehouseCreateView(missing_cfg, cols));
  EXPECT_TRUE(res1->HasError());

  std::string err1;
  EXPECT_FALSE(ExecuteLakehouseCreateTable(*conn_, missing_cfg, cols, &err1));
  EXPECT_FALSE(err1.empty());
  EXPECT_FALSE(LakehouseCatalog::Instance().IsLakehouseTable("lake", "bad_tbl"));

  std::string valid_paimon =
      std::string(PAIMON_GOLDEN_DATA_DIR) + "/parquet/append_09.db/append_09";
  auto bad_col_cfg = ParseLakehouseUri("lake", "bad_col_tbl", "paimon://" + valid_paimon);
  std::vector<LakehouseColumnDecl> wrong_cols = {{"non_existent_column_xyz", "INTEGER"}};
  auto res2 = conn_->Query(TranslateLakehouseCreateView(bad_col_cfg, wrong_cols));
  EXPECT_TRUE(res2->HasError());

  std::string err2;
  EXPECT_FALSE(ExecuteLakehouseCreateTable(*conn_, bad_col_cfg, wrong_cols, &err2));
  EXPECT_FALSE(err2.empty());
  EXPECT_FALSE(LakehouseCatalog::Instance().IsLakehouseTable("lake", "bad_col_tbl"));
}

TEST_F(LakehouseE2ETest, ExecutesFederatedJoinAcrossIcebergAndPaimonThroughMariadbPegDialectAst) {
  // 1. Create MariaDB ENGINE=DuckDB lakehouse view over Apache Iceberg v2 table
  std::string ice_path = std::string(LAKEHOUSE_FIXTURE_DIR) + "/iceberg_v2_orders";
  auto ice_cfg = ParseLakehouseUri("lake", "orders", "iceberg://" + ice_path);
  std::vector<LakehouseColumnDecl> ice_cols = {
      {"order_id", "BIGINT"},
      {"f1", "INTEGER"},
      {"region", "VARCHAR"},
      {"amount", "DOUBLE"},
  };
  std::string err;
  ASSERT_TRUE(ExecuteLakehouseCreateTable(*conn_, ice_cfg, ice_cols, &err)) << err;
  EXPECT_TRUE(LakehouseCatalog::Instance().IsLakehouseTable("lake", "orders"));

  // 2. Create MariaDB ENGINE=DuckDB lakehouse view over Apache Paimon table
  std::string paimon_path =
      std::string(PAIMON_GOLDEN_DATA_DIR) + "/parquet/append_09.db/append_09";
  auto pai_cfg = ParseLakehouseUri("lake", "paimon_dim", "paimon://" + paimon_path);
  std::vector<LakehouseColumnDecl> pai_cols = {
      {"f0", "VARCHAR"},
      {"f1", "INTEGER"},
      {"f2", "INTEGER"},
      {"f3", "DOUBLE"},
  };
  ASSERT_TRUE(ExecuteLakehouseCreateTable(*conn_, pai_cfg, pai_cols, &err)) << err;
  EXPECT_TRUE(LakehouseCatalog::Instance().IsLakehouseTable("lake", "paimon_dim"));

  // 3. Parse a MariaDB backtick-quoted SQL query joining Iceberg and Paimon
  //    through the engine's exact MariadbDialect PEG AST pipeline and stream results
  std::string mariadb_sql =
      "SELECT `o`.`order_id`, `o`.`region`, `p`.`f0`, `o`.`amount` "
      "FROM `lake`.`orders` AS `o` "
      "JOIN `lake`.`paimon_dim` AS `p` ON `o`.`f1` = `p`.`f1` "
      "WHERE `o`.`region` = 'APAC' "
      "ORDER BY `o`.`order_id`, `p`.`f0` LIMIT 5";

  ASSERT_FALSE(myduck::mariadb_query_has_lexical_mismatch(mariadb_sql, true, false));
  auto ast = myduck::parse_mariadb_statement(*conn_, mariadb_sql);
  ASSERT_NE(ast, nullptr);

  auto stream = conn_->Submit(std::move(ast));
  ASSERT_NE(stream, nullptr);
  auto chunk = stream->Fetch();
  ASSERT_NE(chunk, nullptr);
  EXPECT_FALSE(stream->HasError()) << stream->GetError();
  EXPECT_GT(chunk->size(), 0u);
  EXPECT_EQ(chunk->GetValue(0, 0).GetValue<int64_t>(), 101);
  EXPECT_EQ(chunk->GetValue(1, 0).ToString(), "APAC");

  // 4. Verify drop unregisters from catalog and drops the DuckDB view
  ASSERT_TRUE(ExecuteLakehouseDropTable(*conn_, "lake", "orders", &err)) << err;
  EXPECT_FALSE(LakehouseCatalog::Instance().IsLakehouseTable("lake", "orders"));
  ASSERT_TRUE(ExecuteLakehouseDropTable(*conn_, "lake", "paimon_dim", &err)) << err;
  EXPECT_FALSE(LakehouseCatalog::Instance().IsLakehouseTable("lake", "paimon_dim"));
}

TEST_F(LakehouseE2ETest, ExecutesIcebergRestEndToEndCreateQueryAndDrop) {
  std::string ice_path = std::string(LAKEHOUSE_FIXTURE_DIR) + "/iceberg_v2_orders";
  std::string meta_loc = ice_path + "/metadata/v1.metadata.json";

  duckdb_httplib::Server svr;
  svr.Get("/v1/config", [](const duckdb_httplib::Request &, duckdb_httplib::Response &res) {
    res.set_content(R"({"defaults":{},"overrides":{}})", "application/json");
  });
  svr.Get("/v1/namespaces/analytics/tables/orders",
          [&](const duckdb_httplib::Request &, duckdb_httplib::Response &res) {
            std::string body = R"({"metadata-location":")" + meta_loc + R"(","metadata":{}})";
            res.set_content(body, "application/json");
          });

  int port = svr.bind_to_any_port("127.0.0.1");
  ASSERT_GT(port, 0);
  std::thread server_thread([&]() { svr.listen_after_bind(); });
  struct ServerGuard {
    duckdb_httplib::Server &s;
    std::thread &t;
    ~ServerGuard() {
      s.stop();
      if (t.joinable()) {
        t.join();
      }
    }
  } guard{svr, server_thread};

  std::string rest_uri =
      "iceberg_rest://prod_cat/analytics.orders?endpoint=http://127.0.0.1:" +
      std::to_string(port) + "&warehouse=wh";
  auto rest_cfg = ParseLakehouseUri("lake", "rest_orders", rest_uri);
  std::vector<LakehouseColumnDecl> ice_cols = {
      {"order_id", "BIGINT"},
      {"region", "VARCHAR"},
      {"amount", "DOUBLE"},
  };

  std::string err;
  ASSERT_TRUE(ExecuteLakehouseCreateTable(*conn_, rest_cfg, ice_cols, &err)) << err;
  EXPECT_TRUE(LakehouseCatalog::Instance().IsLakehouseTable("lake", "rest_orders"));

  std::string mariadb_sql =
      "SELECT `order_id`, `amount` FROM `lake`.`rest_orders` "
      "WHERE `region` = 'APAC' ORDER BY `order_id`";
  ASSERT_FALSE(myduck::mariadb_query_has_lexical_mismatch(mariadb_sql, true, false));
  auto ast = myduck::parse_mariadb_statement(*conn_, mariadb_sql);
  ASSERT_NE(ast, nullptr);

  auto stream = conn_->Submit(std::move(ast));
  ASSERT_NE(stream, nullptr);
  auto chunk = stream->Fetch();
  ASSERT_NE(chunk, nullptr);
  EXPECT_FALSE(stream->HasError()) << stream->GetError();
  ASSERT_EQ(chunk->size(), 2u);
  EXPECT_EQ(chunk->GetValue(0, 0).GetValue<int64_t>(), 101);

  ASSERT_TRUE(ExecuteLakehouseDropTable(*conn_, "lake", "rest_orders", &err)) << err;
  EXPECT_FALSE(LakehouseCatalog::Instance().IsLakehouseTable("lake", "rest_orders"));
}

}  // namespace
}  // namespace myduck::lakehouse
