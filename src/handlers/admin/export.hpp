#pragma once
#include "http/router.hpp"
#include <string>
namespace examvan::handlers::admin {
std::string build_csv_export(const std::string& exam_name);
std::string build_xlsx_placeholder(const std::string& exam_name);
Response export_submissions_csv(const Request& req);
Response export_submissions_xlsx(const Request& req);
} // namespace examvan::handlers::admin
