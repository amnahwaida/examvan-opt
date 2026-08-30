#include "utils/log.hpp"
#include <mutex>

namespace examvan::utils {

static LogSink g_log_sink;
static std::mutex g_log_mu;

void set_log_sink_for_test(LogSink sink){
  std::lock_guard<std::mutex> g(g_log_mu);
  g_log_sink = std::move(sink);
}

static void emit(const std::string& level, const std::string& event, const std::string& msg){
  std::string line = "[examvan:" + level + "] event=" + event;
  if(!msg.empty()) line += " msg=" + msg;
  std::lock_guard<std::mutex> g(g_log_mu);
  if(g_log_sink){ g_log_sink(line); }
  else { fprintf(stderr, "%s\n", line.c_str()); }
}

void log_info(const std::string& event, const std::string& msg){
  emit("info", event, msg);
}
void log_warn(const std::string& event, const std::string& msg){
  emit("warn", event, msg);
}
void log_error(const std::string& event, const std::string& msg){
  emit("error", event, msg);
}

} // namespace examvan::utils