#pragma once
#include <string>

namespace examvan {

std::string sanitize_ws_field(const std::string& raw, size_t max_len);
std::string sanitize_ws_mac(const std::string& raw);
std::string ws_string(const std::string& json_obj, const std::string& key);
std::string html_escape(const std::string& s);

}  // namespace examvan
