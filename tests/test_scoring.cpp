#include <gtest/gtest.h>
#include "middleware/scoring.hpp"
using namespace examvan::scoring;

TEST(Scoring, SingleChoice) {
  Question q; q.number=1; q.type="single_choice"; q.weight=1; q.key="Jakarta";
  std::vector<Question> qs{q};
  std::map<std::string,std::string> ans{{"1","Jakarta"}};
  EXPECT_DOUBLE_EQ(score_submission(qs,ans),100.0);
  ans["1"]="Bogor";
  EXPECT_DOUBLE_EQ(score_submission(qs,ans),0.0);
}

TEST(Scoring, PartialEmpty) {
  EXPECT_DOUBLE_EQ(score_submission({},{}),0.0);
}

TEST(Scoring, ParseQuestions) {
  std::string j=R"([{"number":1,"type":"single_choice","label":"Q?","weight":1}])";
  auto qs=parse_questions(j);
  ASSERT_EQ(qs.size(),1u);
  EXPECT_EQ(qs[0].number,1);
}
