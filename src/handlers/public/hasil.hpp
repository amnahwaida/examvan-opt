#pragma once
#include "http/router.hpp"
#include "config/config.hpp"
#include "models/exam.hpp"
#include <unordered_map>
#include <string>

namespace examvan::handlers::public_ {
Response cek_hasil_page(const Request& req);
Response hasil_page(const Request& req);
Response cek_hasil_api(const Request& req);

void set_exam_for_test(const std::string& token, const models::Exam& exam);
void clear_exams_for_test();
} // namespace examvan::handlers::public_
