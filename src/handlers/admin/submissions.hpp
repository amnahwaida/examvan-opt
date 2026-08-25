#pragma once
#include "http/router.hpp"
namespace examvan::handlers::admin {
Response list_submissions(const Request& req);
Response submission_detail(const Request& req);
Response queue_status(const Request& req);
Response delete_submission(const Request& req);
} // namespace examvan::handlers::admin
