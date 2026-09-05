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

// Nilai submission dari questions_json exam (bukan string kosong hardcoded).
// Nullopt bila tidak ada soal yang bisa dinilai (questions kosong/tidak valid).
std::optional<double> score_submission_json(const std::string& questions_json, const std::map<std::string,std::string>& answers);

} // namespace examvan::scoring
