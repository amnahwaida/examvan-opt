#pragma once
#include <string>
#include <map>
#include <optional>
#include <vector>

namespace examvan::scoring {

struct Question {
  int number{0};
  std::string type;
  double weight{1};
  std::string key;
  std::vector<std::string> options;
  std::string answer;
  bool partial{false};
};

double score_submission(const std::vector<Question>& qs, const std::map<std::string,std::string>& answers);
std::vector<Question> parse_questions(const std::string& json);

} // namespace examvan::scoring
