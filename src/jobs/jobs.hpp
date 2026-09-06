#pragma once
#include <thread>
#include <atomic>
#include <functional>
#include <chrono>
#include <mutex>
#include <condition_variable>

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
  std::mutex mu_;
  std::condition_variable cv_;
  std::thread th_;
};

void run_expiry_job();
void run_approval_cleanup();
void run_access_log_retention();

} // namespace examvan::jobs
