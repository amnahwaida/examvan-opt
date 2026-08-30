#pragma once
#include <string>
#include <functional>
#include <cstdio>

namespace examvan::utils {

using LogSink = std::function<void(const std::string& line)>;

/* Sink saat ini (nullptr = tulis ke stderr seperti biasa). */
void set_log_sink_for_test(LogSink sink);
void log_info(const std::string& event, const std::string& msg="");
void log_warn(const std::string& event, const std::string& msg="");
void log_error(const std::string& event, const std::string& msg="");

} // namespace examvan::utils
