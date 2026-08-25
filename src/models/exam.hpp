#pragma once
#include <string>
#include <optional>
#include <chrono>

namespace examvan::models {

struct Exam {
  int id{0};
  std::string name;
  std::string file_path;
  int64_t size_bytes{0};
  std::string token;
  std::string active_token;
  std::optional<std::string> questions_json;
  std::string status{"inactive"};
  std::string security_level{"medium"};
  int strict_mode{0};
  int public_results{1};
  int show_answers{1};
  int created_by{0};
  std::string created_at;
  std::optional<std::string> identity_fields;
  std::optional<std::string> panel_color;
  std::optional<std::string> congrats_message;
  std::optional<std::string> start_time;
  std::optional<std::string> end_time;
  std::optional<int> delegated_to;
  std::optional<std::string> token_mode;
  std::optional<int> token_reset_interval;
  std::optional<std::string> token_last_reset_at;
  std::optional<std::string> exam_started_at;
  bool auto_approve{false};
  std::optional<std::string> tombstoned_at;

  bool is_active() const { return status == "active"; }
  bool is_strict() const { return strict_mode != 0; }
  bool are_results_public() const { return public_results != 0; }
  std::string get_token_mode() const {
    if (!token_mode || token_mode->empty()) return "dynamic";
    return *token_mode;
  }
};

inline constexpr int kSubmissionGraceSeconds = 60;

inline const char* kDefaultExamColumns = "id, name, file_path, size_bytes, token, active_token, questions_json, status, security_level, strict_mode, public_results, show_answers, created_by, created_at, identity_fields, panel_color, start_time, end_time, delegated_to, token_mode, token_reset_interval, token_last_reset_at, exam_started_at, tombstoned_at, congrats_message, auto_approve";

} // namespace examvan::models
