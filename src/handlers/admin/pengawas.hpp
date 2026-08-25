#pragma once
#include "http/router.hpp"
namespace examvan::handlers::admin {
Response pengawas_exams(const Request& req);
Response pengawas_submissions(const Request& req);
Response pending_approvals(const Request& req);
Response set_approval(const Request& req);
Response get_auto_approve(const Request& req);
Response set_auto_approve(const Request& req);
} // namespace examvan::handlers::admin
