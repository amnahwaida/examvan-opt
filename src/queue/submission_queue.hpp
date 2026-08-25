#pragma once
#include <string>
#include <map>
#include <optional>
#include <functional>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>

namespace examvan::queue {

inline constexpr const char* kQueueKey = "examvan:submissions:pending";
inline constexpr const char* kHeartbeatQueueKey = "examvan:heartbeats:pending";
inline constexpr const char* kResultKeyPrefix = "examvan:submissions:result:";
inline constexpr int kMaxRetries = 3;
inline constexpr int kWorkerCount = 8;
inline constexpr int kBatchSize = 50;

struct SubmissionJob {
  std::string job_id;
  int exam_id{0};
  std::string student_name;
  std::string exam_number;
  std::string student_class;
  std::string start_time;
  std::string mac_address;
  std::map<std::string,std::string> answers;
  std::map<std::string,std::string> identity_data;
  int retries{0};
  std::string enqueued_at;
  std::string to_json() const;
  static std::optional<SubmissionJob> from_json(const std::string& s);
};

struct JobResult {
  std::string job_id;
  bool success{false};
  std::optional<double> score;
  std::string message;
  std::string processed_at;
  std::string to_json() const;
};

std::string generate_job_id();

class SubmissionQueue {
public:
  explicit SubmissionQueue(std::function<void(const std::string&,const std::string&)> lpush,
                           std::function<std::optional<std::string>(const std::string&,int)> brpop,
                           std::function<void(const std::string&,const std::string&)> set_result);
  std::string enqueue(const std::map<std::string,std::string>& data);
  std::optional<SubmissionJob> dequeue(int timeout_sec=5);
  void store_result(const JobResult& r);
private:
  std::function<void(const std::string&,const std::string&)> lpush_;
  std::function<std::optional<std::string>(const std::string&,int)> brpop_;
  std::function<void(const std::string&,const std::string&)> set_;
};

class Worker {
public:
  Worker(SubmissionQueue* q, std::function<std::optional<double>(const SubmissionJob&)> scorer);
  void start();
  void stop();
  size_t pending() const;
private:
  void run_worker(int id);
  void run_batch();
  SubmissionQueue* queue_;
  std::function<std::optional<double>(const SubmissionJob&)> scorer_;
  std::atomic<bool> running_{false};
  std::vector<std::thread> workers_;
  std::thread batch_th_;
  std::queue<SubmissionJob> batch_q_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
};

} // namespace examvan::queue
