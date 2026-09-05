#include <gtest/gtest.h>
#include "middleware/scoring.hpp"
using namespace examvan::scoring;

// Go evaluateSingleQuestion parity for the public hasil detail modal:
// earned/status per question must reflect partial_scoring for
// multiple_choice & matching (proportional), not naive whole-string match.

TEST(ScoringDetail, SingleChoice_ExactNorm) {
  Question q; q.number=1; q.type="single_choice"; q.weight=2; q.key="A"; q.key_raw="\"A\"";
  auto d=evaluate_question_detail(q, " a ");
  EXPECT_DOUBLE_EQ(d.earned, 2.0);
  EXPECT_EQ(d.status, "correct");
  auto w=evaluate_question_detail(q, "B");
  EXPECT_DOUBLE_EQ(w.earned, 0.0);
  EXPECT_EQ(w.status, "incorrect");
  auto u=evaluate_question_detail(q, "");
  EXPECT_DOUBLE_EQ(u.earned, 0.0);
  EXPECT_EQ(u.status, "unanswered");
}

TEST(ScoringDetail, MultipleChoice_ExactWithoutPartial) {
  Question q; q.number=2; q.type="multiple_choice"; q.weight=4;
  q.key_raw="[\"A\",\"B\"]"; q.partial=false;
  auto ok=evaluate_question_detail(q, "[\"B\",\"A\"]"); // set sama, urut beda
  EXPECT_DOUBLE_EQ(ok.earned, 4.0);
  EXPECT_EQ(ok.status, "correct");
  auto bad=evaluate_question_detail(q, "[\"A\",\"C\"]");
  EXPECT_DOUBLE_EQ(bad.earned, 0.0);
  EXPECT_EQ(bad.status, "incorrect");
  auto extra=evaluate_question_detail(q, "[\"A\"]");
  EXPECT_DOUBLE_EQ(extra.earned, 0.0);
  EXPECT_EQ(extra.status, "incorrect");
}

TEST(ScoringDetail, MultipleChoice_PartialPenalty) {
  // Go evaluateMC partial: portion = max(0, benarSalahNetto)/banyakKunci.
  Question q; q.number=3; q.type="multiple_choice"; q.weight=6;
  q.key_raw="[\"A\",\"B\",\"C\"]"; q.partial=true;
  // Pilih A(benar) dan D(salah): portion = (1-1)/3 = 0 → salah.
  auto zero=evaluate_question_detail(q, "[\"A\",\"D\"]");
  EXPECT_DOUBLE_EQ(zero.earned, 0.0);
  EXPECT_EQ(zero.status, "incorrect");
  // Pilih A,C (benar) + D (salah): portion=(2-1)/3=1/3 → earned 2, status partial.
  auto part=evaluate_question_detail(q, "[\"A\",\"C\",\"D\"]");
  EXPECT_NEAR(part.earned, 2.0, 1e-9);
  EXPECT_EQ(part.status, "partial");
  // Benar semua → correct penuh.
  auto full=evaluate_question_detail(q, "[\"C\",\"A\",\"B\"]");
  EXPECT_DOUBLE_EQ(full.earned, 6.0);
  EXPECT_EQ(full.status, "correct");
}

TEST(ScoringDetail, Matching_Partial) {
  // Go evaluateMatching partial: portion = cocok/banyakPasanganKunci.
  Question q; q.number=4; q.type="matching"; q.weight=4;
  q.key_raw="{\"1\":\"A\",\"2\":\"B\",\"3\":\"C\"}"; q.partial=true;
  auto two=evaluate_question_detail(q, "{\"1\":\"A\",\"2\":\"B\",\"3\":\"X\"}");
  EXPECT_NEAR(two.earned, 4.0*2.0/3.0, 1e-9);
  EXPECT_EQ(two.status, "partial");
  auto full=evaluate_question_detail(q, "{\"3\":\"C\",\"1\":\"A\",\"2\":\"B\"}");
  EXPECT_DOUBLE_EQ(full.earned, 4.0);
  EXPECT_EQ(full.status, "correct");
  auto none=evaluate_question_detail(q, "{\"1\":\"Z\"}");
  EXPECT_DOUBLE_EQ(none.earned, 0.0);
  EXPECT_EQ(none.status, "incorrect");
}

TEST(ScoringDetail, ParseQuestions_CapturesKeyRawAndPartial) {
  std::string j=R"([{"number":1,"type":"multiple_choice","weight":4,"partial_scoring":true,"key":["A","B"],"choices":["A","B","C","D"]},
                    {"number":2,"type":"matching","weight":2,"partial_scoring":false,"key":{"1":"A","2":"B"}}])";
  auto qs=parse_questions(j);
  ASSERT_EQ(qs.size(), 2u);
  EXPECT_EQ(qs[0].type, "multiple_choice");
  EXPECT_TRUE(qs[0].partial);
  EXPECT_NE(qs[0].key_raw.find("\"A\""), std::string::npos);
  EXPECT_NE(qs[0].key_raw.find("\"B\""), std::string::npos);
  EXPECT_EQ(qs[1].type, "matching");
  EXPECT_FALSE(qs[1].partial);
  EXPECT_NE(qs[1].key_raw.find("\"1\":\"A\""), std::string::npos);
}