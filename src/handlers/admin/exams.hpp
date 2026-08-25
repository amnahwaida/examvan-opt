#pragma once
#include "http/router.hpp"
namespace examvan::handlers::admin {
Response list_admin_exams(const Request& req);
Response create_exam(const Request& req);
Response update_exam(const Request& req);
Response delete_exam(const Request& req);
Response export_xlsx(const Request& req);
} // namespace examvan::handlers::admin
