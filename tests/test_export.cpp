#include <gtest/gtest.h>
#include "handlers/admin/export.hpp"
using namespace examvan::handlers::admin;

TEST(Export, CsvContainsHeader) {
  auto csv=build_csv_export("UAS");
  EXPECT_NE(csv.find("exam,student"), std::string::npos);
  EXPECT_NE(csv.find("UAS"), std::string::npos);
}

TEST(Export, XlsxPlaceholderHasPK) {
  auto x=build_xlsx_placeholder("UAS");
  EXPECT_EQ(x.substr(0,2), "PK");
}

TEST(Export, HandlersReturn200) {
  examvan::Request r;
  EXPECT_EQ(export_submissions_csv(r).status, 200);
  EXPECT_EQ(export_submissions_xlsx(r).status, 200);
  EXPECT_NE(export_submissions_xlsx(r).headers["Content-Type"].find("spreadsheetml"), std::string::npos);
}
