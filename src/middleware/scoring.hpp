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
  // Nilai mentah kolom "key" dari JSON soal: string ("A"), array (["A","B"])
  // untuk multiple_choice, atau objek ({"1":"A"}) untuk matching.
  std::string key_raw;
};

double score_submission(const std::vector<Question>& qs, const std::map<std::string,std::string>& answers);
std::vector<Question> parse_questions(const std::string& json);

// Nilai submission dari questions_json exam (bukan string kosong hardcoded).
// Nullopt bila tidak ada soal yang bisa dinilai (questions kosong/tidak valid).
std::optional<double> score_submission_json(const std::string& questions_json, const std::map<std::string,std::string>& answers);

// ===== Evaluasi detail per-soal (paritas Go evaluateSingleQuestion) =====
// Dipakai halaman publik hasil untuk modal detail: earned + status per soal
// memperhitungkan partial_scoring (multiple_choice/matching diberi poin
// proporsional), bukan pencocokan string utuh yang keliru.
struct EvalDetail {
  double earned{0};
  std::string status; // "correct" | "partial" | "incorrect" | "unanswered"
};

// answer_raw = nilai jawaban siswa apa adanya (string, array JSON, objek JSON).
EvalDetail evaluate_question_detail(const Question& q, const std::string& answer_raw);

} // namespace examvan::scoring