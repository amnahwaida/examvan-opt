#include <gtest/gtest.h>
#include "handlers/admin/exams.hpp"
using namespace examvan::handlers::admin;

TEST(F6Exams, CreateRequiresName) {
  examvan::Request req; req.body="file_path=/tmp/a.pdf&size_bytes=1000";
  auto r=create_exam(req); EXPECT_EQ(r.status,400);
}

TEST(F6Exams, CreateFileSizeLimit) {
  examvan::Request req; req.body="name=UAS&file_path=/tmp/a.pdf&size_bytes=99999999";
  auto r=create_exam(req); EXPECT_EQ(r.status,413);
}

TEST(F6Exams, CreateSuccess201) {
  examvan::Request req; req.body="name=UAS Matematika&file_path=exams/1/a.pdf&size_bytes=1024000";
  auto r=create_exam(req); EXPECT_EQ(r.status,201);
  EXPECT_NE(r.body.find("UAS Matematika"), std::string::npos);
}

TEST(F6Exams, ExportXlsxHeaders) {
  examvan::Request req;
  auto r=export_xlsx(req);
  // ExportXlsx belum diimplementasi — 501 Not Implemented (bukan 200 fake xlsx)
  EXPECT_EQ(r.status,501);
  EXPECT_NE(r.body.find("NOT_IMPLEMENTED"), std::string::npos);
}
