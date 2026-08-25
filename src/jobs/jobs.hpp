#pragma once
#include <thread>
#include <atomic>
#include <functional>
#include <chrono>

namespace examvan::jobs {

class JobRunner {
public:
  explicit JobRunner(std::function<void()> fn, std::chrono::seconds interval);
  ~JobRunner();
  void start();
  void stop();
private:
  std::function<void()> fn_;
  std::chrono::seconds interval_;
  std::atomic<bool> running_{false};
  std::thread th_;
};

void run_expiry_job();
void run_approval_cleanup();
void run_access_log_retention();

} // namespace examvan::jobs
