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
  // Export XLSX nyata: 200 + zip valid + header kolom paritas Go.
  EXPECT_EQ(r.status,200) << r.body.substr(0,100);
  EXPECT_EQ(r.body.substr(0,2), "PK");
  EXPECT_NE(r.body.find("Nama Siswa"), std::string::npos);
}
