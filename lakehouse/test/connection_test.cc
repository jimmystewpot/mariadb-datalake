// lakehouse/test/connection_test.cc
#include <gtest/gtest.h>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include "lakehouse/lakehouse_connection.h"

namespace myduck::lakehouse {
namespace {

TEST(ConnectionTest, ParsesPaimonIcebergAndRestUrisWithOptions) {
  SetAllowedPathPrefix("");
  auto paimon_cfg = ParseLakehouseUri(
      "analytics", "users",
      "paimon:///var/data/warehouse/db.db/users?snapshot_id=5");
  EXPECT_EQ(paimon_cfg.format, LakehouseFormat::kPaimonPath);
  EXPECT_EQ(paimon_cfg.table_path, "/var/data/warehouse/db.db/users");
  EXPECT_EQ(paimon_cfg.snapshot_id, 5);
  EXPECT_EQ(BuildDuckDBScanSql(paimon_cfg),
            "paimon_scan('/var/data/warehouse/db.db/users', snapshot_id := 5)");

  auto ice_cfg = ParseLakehouseUri(
      "analytics", "orders",
      "iceberg://s3://bucket/warehouse/db/orders?version=v2&allow_moved_paths=true");
  EXPECT_EQ(ice_cfg.format, LakehouseFormat::kIcebergPath);
  EXPECT_EQ(ice_cfg.table_path, "s3://bucket/warehouse/db/orders");
  ASSERT_TRUE(ice_cfg.iceberg_version.has_value());
  EXPECT_EQ(*ice_cfg.iceberg_version, "v2");
  EXPECT_EQ(BuildDuckDBScanSql(ice_cfg),
            "iceberg_scan('s3://bucket/warehouse/db/orders', allow_moved_paths := true, version := 'v2')");

  auto rest_cfg = ParseLakehouseUri(
      "analytics", "events", "",
      "iceberg_rest://lake_cat/web.events?endpoint=http://127.0.0.1:18181&warehouse=wh");
  EXPECT_EQ(rest_cfg.format, LakehouseFormat::kIcebergRest);
  EXPECT_EQ(rest_cfg.rest_catalog, "lake_cat");
  EXPECT_EQ(rest_cfg.rest_namespace, "web");
  EXPECT_EQ(rest_cfg.rest_table, "events");
  EXPECT_EQ(rest_cfg.rest_endpoint, "http://127.0.0.1:18181");
  EXPECT_EQ(rest_cfg.rest_warehouse, "wh");

  // Multi-level REST namespace splits at the last dot (rfind('.'))
  auto multi_ns_cfg = ParseLakehouseUri(
      "analytics", "orders",
      "iceberg_rest://lake_cat/org.team.orders?endpoint=https://127.0.0.1:18181&warehouse=wh&version=v2");
  EXPECT_EQ(multi_ns_cfg.format, LakehouseFormat::kIcebergRest);
  EXPECT_EQ(multi_ns_cfg.rest_catalog, "lake_cat");
  EXPECT_EQ(multi_ns_cfg.rest_namespace, "org.team");
  EXPECT_EQ(multi_ns_cfg.rest_table, "orders");
  ASSERT_TRUE(multi_ns_cfg.iceberg_version.has_value());
  EXPECT_EQ(*multi_ns_cfg.iceberg_version, "v2");
}

TEST(ConnectionTest, RejectsPlaintextSecretsAndPathTraversal) {
  SetAllowedPathPrefix("");
  auto secret_uri = ParseLakehouseUri(
      "db", "t", "paimon:///var/data/wh/db.db/t?secret_key=my_secret");
  EXPECT_EQ(secret_uri.format, LakehouseFormat::kInvalid);
  EXPECT_NE(secret_uri.error_message.find("credential"), std::string::npos);

  // Rejects authority userinfo credentials (user:pass@) in table paths and REST endpoints
  auto s3_userinfo = ParseLakehouseUri(
      "db", "t", "iceberg://s3://user:pass@bucket/wh/db/t");
  EXPECT_EQ(s3_userinfo.format, LakehouseFormat::kInvalid);
  EXPECT_NE(s3_userinfo.error_message.find("credential"), std::string::npos);

  auto rest_ep_userinfo = ParseLakehouseUri(
      "db", "t",
      "iceberg_rest://cat/ns.tbl?endpoint=http://user:pass@127.0.0.1:18181");
  EXPECT_EQ(rest_ep_userinfo.format, LakehouseFormat::kInvalid);
  EXPECT_NE(rest_ep_userinfo.error_message.find("credential"), std::string::npos);

  auto rest_cat_userinfo = ParseLakehouseUri(
      "db", "t",
      "iceberg_rest://user:pass@cat/ns.tbl?endpoint=http://127.0.0.1:18181");
  EXPECT_EQ(rest_cat_userinfo.format, LakehouseFormat::kInvalid);
  EXPECT_NE(rest_cat_userinfo.error_message.find("credential"), std::string::npos);

  // Rejects path traversal in table paths and REST endpoints
  auto traversal_uri = ParseLakehouseUri(
      "db", "t", "iceberg:///var/data/../../etc/passwd");
  EXPECT_EQ(traversal_uri.format, LakehouseFormat::kInvalid);

  auto rest_ep_traversal = ParseLakehouseUri(
      "db", "t",
      "iceberg_rest://cat/ns.tbl?endpoint=http://127.0.0.1:18181/v1/../admin");
  EXPECT_EQ(rest_ep_traversal.format, LakehouseFormat::kInvalid);

  // Rejects paimon://s3://... because PAIMON_ENABLE_S3=OFF, while allowing iceberg://s3://...
  auto paimon_s3 = ParseLakehouseUri("db", "t", "paimon://s3://bucket/wh/db.db/t");
  EXPECT_EQ(paimon_s3.format, LakehouseFormat::kInvalid);
  EXPECT_NE(paimon_s3.error_message.find("local filesystem"), std::string::npos);

  SetAllowedPathPrefix("/var/allowed");
  auto disallowed = ParseLakehouseUri("db", "t", "paimon:///tmp/other/db.db/t");
  EXPECT_EQ(disallowed.format, LakehouseFormat::kInvalid);
  SetAllowedPathPrefix("");
}

TEST(ConnectionTest, TranslatesCreateViewWithColumnCastsAndIdentifierQuoting) {
  auto cfg = ParseLakehouseUri("sales", "orders", "paimon:///data/wh/sales.db/orders");
  std::vector<LakehouseColumnDecl> cols = {
      {"order_id", "BIGINT"},
      {"region", "VARCHAR COLLATE NOCASE"},
      {"amount", "DECIMAL(10,2)"},
  };
  std::string ddl = TranslateLakehouseCreateView(cfg, cols);
  EXPECT_NE(ddl.find("CREATE SCHEMA IF NOT EXISTS \"sales\";"), std::string::npos) << ddl;
  EXPECT_NE(ddl.find("CREATE OR REPLACE VIEW \"sales\".\"orders\" (\"order_id\", \"region\", \"amount\") AS"),
            std::string::npos) << ddl;
  EXPECT_NE(ddl.find("CAST(\"region\" AS VARCHAR) AS \"region\""), std::string::npos) << ddl;
  EXPECT_NE(ddl.find("FROM paimon_scan('/data/wh/sales.db/orders')"), std::string::npos) << ddl;
}

TEST(ConnectionTest, HandlesEdgeCasesEscapingAndPrefixBoundaries) {
  SetAllowedPathPrefix("");
  // Non-lakehouse tables (empty connection and non-URI comment) return kNone
  EXPECT_EQ(ParseLakehouseUri("db", "t", "", "").format, LakehouseFormat::kNone);
  EXPECT_EQ(ParseLakehouseUri("db", "t", "", "regular table comment").format,
            LakehouseFormat::kNone);

  // Relative local paths are rejected
  EXPECT_EQ(ParseLakehouseUri("db", "t", "paimon://relative/wh/db.db/t").format,
            LakehouseFormat::kInvalid);

  // Prefix boundary check prevents sibling directory bypass (/var/allowed_other)
  SetAllowedPathPrefix("/var/allowed");
  EXPECT_EQ(ParseLakehouseUri("db", "t", "paimon:///var/allowed_other/db.db/t").format,
            LakehouseFormat::kInvalid);
  EXPECT_EQ(ParseLakehouseUri("db", "t", "paimon:///var/allowed/db.db/t").format,
            LakehouseFormat::kPaimonPath);
  SetAllowedPathPrefix("");

  // All forbidden credential keys are rejected
  for (const char *key :
       {"password", "secret", "token", "access_key", "secret_key", "session_token"}) {
    auto cfg = ParseLakehouseUri(
        "db", "t", std::string("iceberg:///var/wh/t?") + key + "=val");
    EXPECT_EQ(cfg.format, LakehouseFormat::kInvalid) << key;
    EXPECT_NE(cfg.error_message.find("credential"), std::string::npos) << key;
  }

  // Single quotes in paths and double quotes in identifiers are escaped
  auto quote_cfg =
      ParseLakehouseUri("sa\"les", "or\"ders", "paimon:///data/o'reilly/sales.db/orders");
  ASSERT_EQ(quote_cfg.format, LakehouseFormat::kPaimonPath);
  EXPECT_EQ(BuildDuckDBScanSql(quote_cfg),
            "paimon_scan('/data/o''reilly/sales.db/orders')");
  std::string quoted_ddl =
      TranslateLakehouseCreateView(quote_cfg, {{"co\"l", "VARCHAR COLLATE POSIX"}});
  EXPECT_NE(quoted_ddl.find("\"sa\"\"les\".\"or\"\"ders\" (\"co\"\"l\")"),
            std::string::npos)
      << quoted_ddl;
}

TEST(ConnectionTest, TightHeapBuffersAtMisalignedOffsetsAboveSimdThreshold) {
  SetAllowedPathPrefix("");
  const std::string long_path =
      "/tmp/" + std::string(280, 'a') + "/warehouse/db.db/orders";
  const std::string valid_uri = "paimon://" + long_path;
  const std::string invalid_uri =
      "paimon:///tmp/" + std::string(280, 'b') + "/../secret";

  for (size_t offset = 0; offset < 16; ++offset) {
    std::unique_ptr<char[]> tight_valid(new char[offset + valid_uri.size()]);
    if (offset > 0) {
      std::memset(tight_valid.get(), 'X', offset);
    }
    std::memcpy(tight_valid.get() + offset, valid_uri.data(), valid_uri.size());
    std::string_view valid_view(tight_valid.get() + offset, valid_uri.size());

    auto cfg = ParseLakehouseUri("analytics", "orders", valid_view);
    EXPECT_EQ(cfg.format, LakehouseFormat::kPaimonPath);
    EXPECT_EQ(cfg.table_path, long_path);

    auto comment_cfg = ParseLakehouseUri("analytics", "orders", "", valid_view);
    EXPECT_EQ(comment_cfg.format, LakehouseFormat::kPaimonPath);
    EXPECT_EQ(comment_cfg.table_path, long_path);

    std::unique_ptr<char[]> tight_invalid(new char[offset + invalid_uri.size()]);
    if (offset > 0) {
      std::memset(tight_invalid.get(), 'X', offset);
    }
    std::memcpy(tight_invalid.get() + offset, invalid_uri.data(),
                invalid_uri.size());
    std::string_view invalid_view(tight_invalid.get() + offset,
                                  invalid_uri.size());

    auto bad_cfg = ParseLakehouseUri("analytics", "orders", invalid_view);
    EXPECT_EQ(bad_cfg.format, LakehouseFormat::kInvalid);
    EXPECT_NE(bad_cfg.error_message.find("traversal"), std::string::npos);
  }
}

}  // namespace
}  // namespace myduck::lakehouse
