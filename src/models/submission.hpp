#pragma once
#include <string>
#include <optional>

namespace examvan::models {

struct Submission {
  int id{0};
  int exam_id{0};
  std::string student_name;
  std::string exam_number;
  std::string student_class;
  std::optional<std::string> answers_json;
  std::optional<double> score;
  std::string start_time;
  std::string mac_address;
  std::string created_at;
  std::optional<std::string> identity_data;
};

struct StudentAccessLog {
  int id{0};
  int exam_id{0};
  std::optional<int> submission_id;
  std::string student_identifier;
  std::string event;
  std::string ip_address;
  std::string device_info;
  std::string created_at;
};

inline const char* kSubmissionColumns = "id, exam_id, student_name, exam_number, student_class, answers_json, score, start_time, mac_address, created_at, identity_data";

} // namespace examvan::models
