#pragma once
#include "http/router.hpp"
#include <string>
#include <vector>
namespace examvan::handlers::admin {
struct SubmissionRow {
  std::string id;
  std::string exam_name;
  std::string student_name;
  std::string exam_number;
  std::string student_class;
  std::string score;
  std::string submitted_at;
  std::string mac;
};
std::string build_csv_export(const std::string& exam_name);
std::string build_submissions_xlsx(const std::vector<SubmissionRow>& rows);
Response export_submissions_csv(const Request& req);
Response export_submissions_xlsx(const Request& req);
} // namespace examvan::handlers::admin
