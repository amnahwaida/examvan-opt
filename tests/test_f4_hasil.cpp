#include <gtest/gtest.h>
#include "handlers/public/hasil.hpp"
#include "models/exam.hpp"
using namespace examvan::handlers::public_;
using namespace examvan::models;

TEST(F4Hasil, NotFoundShowsNeutralTitle) {
  clear_exams_for_test();
  examvan::Request req; req.params["token"]="NOTFOUND";
  auto res=hasil_page(req);
  EXPECT_EQ(res.status,404);
  EXPECT_NE(res.body.find("<h1>Hasil Ujian</h1>"), std::string::npos);
  EXPECT_EQ(res.body.find("id=\"examTitle\""), std::string::npos);
}

TEST(F4Hasil, DisabledWhenNotPublic) {
  clear_exams_for_test();
  Exam e; e.token="TOK123"; e.name="UAS"; e.public_results=0;
  set_exam_for_test("TOK123", e);
  examvan::Request req; req.params["token"]="TOK123";
  auto res=hasil_page(req);
  EXPECT_EQ(res.status,200);
  EXPECT_NE(res.body.find("Hasil belum dipublikasikan"), std::string::npos);
  EXPECT_EQ(res.body.find("id=\"examTitle\""), std::string::npos);
}

TEST(F4Hasil, SuccessShowsExamTitle) {
  clear_exams_for_test();
  Exam e; e.token="TOK999"; e.name="UAS Matematika"; e.public_results=1;
  set_exam_for_test("TOK999", e);
  examvan::Request req; req.params["token"]="TOK999";
  auto res=hasil_page(req);
  EXPECT_EQ(res.status,200);
  EXPECT_NE(res.body.find("id=\"examTitle\""), std::string::npos);
  EXPECT_NE(res.body.find("UAS Matematika"), std::string::npos);
  clear_exams_for_test();
}

TEST(F4Hasil, CekHasilHasAriaLabel) {
  examvan::Request req;
  auto res=cek_hasil_page(req);
  EXPECT_NE(res.body.find("cekHasilForm"), std::string::npos);
}
