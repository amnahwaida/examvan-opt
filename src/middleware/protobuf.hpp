#pragma once
#include "http/router.hpp"
#include "config/config.hpp"
#include <optional>
namespace examvan::middleware {
bool is_protobuf_content(const Request& req);
bool is_protobuf_accept(const Request& req);
std::optional<Response> require_protobuf(const Request& req, const Config& cfg);
std::string protobuf_error_json(const std::string& code, const std::string& msg);
}
