// lakehouse/test/iceberg_catalog_test.cc
#include <gtest/gtest.h>
#include <thread>
#include "duckdb.hpp"
#include "httplib.hpp"
#include "lakehouse/lakehouse_catalog.h"
#include "lakehouse/lakehouse_connection.h"

extern "C" int32_t duckdb_register_static_extensions(void);

namespace myduck::lakehouse {
namespace {

namespace duckdb_httplib = CPPHTTPLIB_NAMESPACE;

class IcebergCatalogTest : public ::testing::Test {
 protected:
  void SetUp() override {
    duckdb_register_static_extensions();
    LakehouseCatalog::Instance().ClearForTesting();
    db_ = std::make_unique<duckdb::DuckDB>(nullptr);
    conn_ = std::make_unique<duckdb::Connection>(*db_);
    iceberg_fixture_dir_ = std::string(LAKEHOUSE_FIXTURE_DIR) + "/iceberg_v2_orders";
  }
  std::unique_ptr<duckdb::DuckDB> db_;
  std::unique_ptr<duckdb::Connection> conn_;
  std::string iceberg_fixture_dir_;
};

TEST_F(IcebergCatalogTest, QueriesGoldenIcebergV2TableWithFilterAndProjection) {
  auto res = conn_->Query(
      "SELECT order_id, region, amount FROM iceberg_scan('" + iceberg_fixture_dir_ +
      "', allow_moved_paths := true) WHERE region = 'APAC' ORDER BY order_id");
  ASSERT_FALSE(res->HasError()) << res->GetError();
  auto chunk = res->Fetch();
  ASSERT_NE(chunk, nullptr);
  ASSERT_EQ(chunk->size(), 2u);
  EXPECT_EQ(chunk->GetValue(0, 0).GetValue<int64_t>(), 101);
  EXPECT_EQ(chunk->GetValue(1, 0).ToString(), "APAC");
}

TEST_F(IcebergCatalogTest, ResolvesIcebergRestCatalogOverHttpAndTracksTableLifecycle) {
  // Spin up an in-process HTTP server speaking the Iceberg REST Catalog protocol
  duckdb_httplib::Server svr;
  std::string meta_loc = iceberg_fixture_dir_ + "/metadata/v1.metadata.json";

  svr.Get("/v1/config", [](const duckdb_httplib::Request &, duckdb_httplib::Response &res) {
    res.set_content(R"({"defaults":{},"overrides":{}})", "application/json");
  });
  svr.Get("/v1/namespaces/analytics/tables/orders",
          [&](const duckdb_httplib::Request &, duckdb_httplib::Response &res) {
            std::string body = R"({"metadata-location":")" + meta_loc + R"(","metadata":{}})";
            res.set_content(body, "application/json");
          });
  svr.Get("/v1/prefixes/wh/namespaces/analytics/tables/prefixed_orders",
          [&](const duckdb_httplib::Request &, duckdb_httplib::Response &res) {
            // Earlier JSON fields contain escaped "metadata-location" in a value and as a value
            // to verify ExtractJsonStringField's lexical token scanner is never fooled.
            std::string body =
                R"({"note":"ignore fake \"metadata-location\": \"/tmp/../evil\"","alias":"metadata-location","metadata-location":")" +
                meta_loc + R"(","metadata":{}})";
            res.set_content(body, "application/json");
          });
  svr.Get("/v1/namespaces/analytics/tables/traversal_orders",
          [&](const duckdb_httplib::Request &, duckdb_httplib::Response &res) {
            std::string body =
                R"({"metadata-location":"/tmp/wh/../../etc/passwd/metadata/v1.metadata.json"})";
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

  LakehouseTableConfig cfg;
  cfg.format = LakehouseFormat::kIcebergRest;
  cfg.schema_name = "lake";
  cfg.table_name = "rest_orders";
  cfg.rest_catalog = "prod_cat";
  cfg.rest_namespace = "analytics";
  cfg.rest_table = "orders";
  cfg.rest_endpoint = "http://127.0.0.1:" + std::to_string(port);
  cfg.rest_warehouse = "wh";

  std::string resolved_scan_sql;
  std::string err;
  EXPECT_TRUE(LakehouseCatalog::Instance().AttachOrResolveIcebergRest(
      *conn_, cfg, &resolved_scan_sql, &err)) << err;
  EXPECT_NE(resolved_scan_sql.find("iceberg_scan("), std::string::npos);

  auto q_res = conn_->Query("SELECT COUNT(*) FROM " + resolved_scan_sql);
  ASSERT_FALSE(q_res->HasError()) << q_res->GetError();
  EXPECT_EQ(q_res->Fetch()->GetValue(0, 0).GetValue<int64_t>(), 3);

  // Verify warehouse-prefixed REST route (/v1/prefixes/<wh>/namespaces/<ns>/tables/<tbl>)
  LakehouseTableConfig prefixed_cfg = cfg;
  prefixed_cfg.rest_table = "prefixed_orders";
  std::string prefixed_scan_sql;
  EXPECT_TRUE(LakehouseCatalog::Instance().AttachOrResolveIcebergRest(
      *conn_, prefixed_cfg, &prefixed_scan_sql, &err)) << err;
  EXPECT_EQ(prefixed_scan_sql, resolved_scan_sql);

  // Verify error handling when table does not exist in REST catalog
  LakehouseTableConfig missing_cfg = cfg;
  missing_cfg.rest_table = "nonexistent_orders";
  std::string missing_scan_sql;
  EXPECT_FALSE(LakehouseCatalog::Instance().AttachOrResolveIcebergRest(
      *conn_, missing_cfg, &missing_scan_sql, &err));
  EXPECT_FALSE(err.empty());

  // Verify malicious REST metadata-location with '..' traversal is rejected
  LakehouseTableConfig traversal_cfg = cfg;
  traversal_cfg.rest_table = "traversal_orders";
  std::string traversal_scan_sql;
  EXPECT_FALSE(LakehouseCatalog::Instance().AttachOrResolveIcebergRest(
      *conn_, traversal_cfg, &traversal_scan_sql, &err));
  EXPECT_NE(err.find("Path traversal"), std::string::npos) << err;

  // Verify config.iceberg_version is forwarded into resolved_scan_sql
  LakehouseTableConfig versioned_cfg = cfg;
  versioned_cfg.iceberg_version = "1";
  std::string versioned_scan_sql;
  EXPECT_TRUE(LakehouseCatalog::Instance().AttachOrResolveIcebergRest(
      *conn_, versioned_cfg, &versioned_scan_sql, &err)) << err;
  EXPECT_NE(versioned_scan_sql.find("version := '1'"), std::string::npos)
      << versioned_scan_sql;

  // Verify https:// endpoint does not throw an uncaught exception when unreachable
  LakehouseTableConfig https_bad_cfg = cfg;
  https_bad_cfg.rest_endpoint = "https://127.0.0.1:1";
  std::string https_scan_sql;
  EXPECT_FALSE(LakehouseCatalog::Instance().AttachOrResolveIcebergRest(
      *conn_, https_bad_cfg, &https_scan_sql, &err));
  EXPECT_FALSE(err.empty());

  // Verify real HTTPS (TLS) Iceberg REST Catalog resolution via SSLServer
  std::string cert_path = std::string(QUACK_TEST_CERT_DIR) + "/server.pem";
  std::string key_path = std::string(QUACK_TEST_CERT_DIR) + "/private_key.pem";
  duckdb_httplib::SSLServer ssl_svr(cert_path.c_str(), key_path.c_str());
  ASSERT_TRUE(ssl_svr.is_valid());
  ssl_svr.Get("/v1/namespaces/analytics/tables/orders",
              [&](const duckdb_httplib::Request &, duckdb_httplib::Response &res) {
                std::string body =
                    R"({"metadata-location":")" + meta_loc + R"(","metadata":{}})";
                res.set_content(body, "application/json");
              });
  int ssl_port = ssl_svr.bind_to_any_port("127.0.0.1");
  ASSERT_GT(ssl_port, 0);
  std::thread ssl_thread([&]() { ssl_svr.listen_after_bind(); });
  struct SslServerGuard {
    duckdb_httplib::SSLServer &s;
    std::thread &t;
    ~SslServerGuard() {
      s.stop();
      if (t.joinable()) {
        t.join();
      }
    }
  } ssl_guard{ssl_svr, ssl_thread};

  LakehouseTableConfig https_ok_cfg = cfg;
  https_ok_cfg.rest_endpoint = "https://127.0.0.1:" + std::to_string(ssl_port);
  std::string https_ok_scan_sql;
  EXPECT_TRUE(LakehouseCatalog::Instance().AttachOrResolveIcebergRest(
      *conn_, https_ok_cfg, &https_ok_scan_sql, &err)) << err;
  EXPECT_EQ(https_ok_scan_sql, resolved_scan_sql);
  ssl_svr.stop();
  ssl_thread.join();

  // Verify catalog lifecycle tracking (Register, IsLakehouseTable, FindTable, Rename, Unregister, UnregisterSchema)
  LakehouseCatalog::Instance().RegisterTable(cfg);
  EXPECT_TRUE(LakehouseCatalog::Instance().IsLakehouseTable("lake", "rest_orders"));
  auto found1 = LakehouseCatalog::Instance().FindTable("lake", "rest_orders");
  ASSERT_TRUE(found1.has_value());
  EXPECT_EQ(found1->rest_table, "orders");
  LakehouseCatalog::Instance().RenameTable("lake", "rest_orders", "lake", "orders_v2");
  EXPECT_FALSE(LakehouseCatalog::Instance().IsLakehouseTable("lake", "rest_orders"));
  EXPECT_TRUE(LakehouseCatalog::Instance().IsLakehouseTable("lake", "orders_v2"));
  auto found2 = LakehouseCatalog::Instance().FindTable("lake", "orders_v2");
  ASSERT_TRUE(found2.has_value());
  EXPECT_EQ(found2->table_name, "orders_v2");
  LakehouseCatalog::Instance().UnregisterTable("lake", "orders_v2");
  EXPECT_FALSE(LakehouseCatalog::Instance().IsLakehouseTable("lake", "orders_v2"));
  EXPECT_FALSE(LakehouseCatalog::Instance().FindTable("lake", "orders_v2").has_value());

  LakehouseTableConfig s1_t1 = cfg;
  s1_t1.schema_name = "drop_db";
  s1_t1.table_name = "t1";
  LakehouseTableConfig s1_t2 = cfg;
  s1_t2.schema_name = "drop_db";
  s1_t2.table_name = "t2";
  LakehouseTableConfig s2_t1 = cfg;
  s2_t1.schema_name = "keep_db";
  s2_t1.table_name = "t1";
  LakehouseCatalog::Instance().RegisterTable(s1_t1);
  LakehouseCatalog::Instance().RegisterTable(s1_t2);
  LakehouseCatalog::Instance().RegisterTable(s2_t1);
  LakehouseCatalog::Instance().UnregisterSchema("drop_db");
  EXPECT_FALSE(LakehouseCatalog::Instance().IsLakehouseTable("drop_db", "t1"));
  EXPECT_FALSE(LakehouseCatalog::Instance().IsLakehouseTable("drop_db", "t2"));
  EXPECT_TRUE(LakehouseCatalog::Instance().IsLakehouseTable("keep_db", "t1"));
  LakehouseCatalog::Instance().UnregisterSchema("keep_db");

  svr.stop();
  server_thread.join();
}

}  // namespace
}  // namespace myduck::lakehouse
