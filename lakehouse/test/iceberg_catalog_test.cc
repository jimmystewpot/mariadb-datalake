#include <gtest/gtest.h>
#include "lakehouse/lakehouse_catalog.h"

TEST(IcebergCatalogStubTest, Placeholder) {
  EXPECT_FALSE(myduck::lakehouse::LakehouseCatalog::Instance().IsLakehouseTable("db", "tbl"));
}
