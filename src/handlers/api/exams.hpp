#pragma once
#include "http/router.hpp"
#include "config/config.hpp"
namespace examvan::handlers::api {
Response list_exams(const Request& req);
Response request_approval(const Request& req);
Response exam_by_token(const Request& req);
Response exam_pdf(const Request& req);
Response submit_exam(const Request& req);
Response exam_result(const Request& req);
Response access_log(const Request& req);
Response complete_exam(const Request& req);
Response health(const Request& req);
Response time_handler(const Request& req);
} // namespace examvan::handlers::api
