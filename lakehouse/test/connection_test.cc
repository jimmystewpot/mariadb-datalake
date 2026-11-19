#include <gtest/gtest.h>
#include "lakehouse/lakehouse_connection.h"

TEST(ConnectionStubTest, Placeholder) {
  auto cfg = myduck::lakehouse::ParseLakehouseUri("db", "tbl", "");
  EXPECT_EQ(cfg.format, myduck::lakehouse::LakehouseFormat::kNone);
}
