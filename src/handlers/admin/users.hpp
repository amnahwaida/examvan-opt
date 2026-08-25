#pragma once
#include "http/router.hpp"
namespace examvan::handlers::admin {
Response list_users(const Request& req);
Response create_user(const Request& req);
Response edit_user(const Request& req);
Response delete_user(const Request& req);
Response instansi_update(const Request& req);
} // namespace examvan::handlers::admin
