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
#include <cstdio>

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
#ifdef HAS_PROTOBUF
  std::string to_protobuf() const;
  static std::optional<SubmissionJob> from_protobuf(const std::string& s);
#endif
};

struct JobResult {
  std::string job_id;
  int exam_id{0};
  std::string mac_address;
  std::string identity_data;
  // P17-M6: fingerprint identitas terikat (anti-TOFU) — diisi worker dari
  // identity_data job, divalidasi /result bila peminta menyertakan identity.
  std::string identity_hash;
  bool success{false};
  std::optional<double> score;
  std::string message;
  std::string processed_at;
  std::string to_json() const;
};

// Sidik jari kanonis atas peta identitas (key diurutkan std::map).
inline std::string identity_fingerprint(const std::map<std::string,std::string>& m){
  std::string s;
  for(auto& kv: m){ s+=kv.first; s+='\x1f'; s+=kv.second; s+='\x1e'; }
  size_t h=std::hash<std::string>{}(s);
  char buf[17];
  snprintf(buf,sizeof(buf),"%016zx",h);
  return std::string(buf);
}

// P17-L2: backoff eksponensial sebelum requeue (hindari hot loop saat DB down).
// 250ms * 2^retries, cap 5 detik.
inline int retry_backoff_ms(int retries){
  int ms=250;
  for(int i=0;i<retries && ms<5000;i++) ms*=2;
  return ms>5000?5000:ms;
}

// Payload heartbeat dari queue examvan:heartbeats:pending (paritas Go
// heartbeatData di cmd/server/main.go drainHeartbeatsQueue).
struct HeartbeatPayload {
  int exam_id{0};
  std::string mac_address, student_name, exam_number, student_class;
  std::string device_info, ip_address, event, last_seen;
};
// Parse payload JSON heartbeat; nullopt bila exam_id hilang / JSON rusak.
std::optional<HeartbeatPayload> parse_heartbeat_payload(const std::string& json);

// Drain heartbeats:pending → student_access_logs (paritas Go
// drainHeartbeatsQueue: batch 500, max 20 batch/tick, requeue saat gagal).
// Connect Redis + PG sendiri; kembalikan total baris yang di-flush.
int drain_heartbeats_once();

// Background flusher (tick 30s, paritas Go startHeartbeatFlusher).
class HeartbeatFlusher {
public:
  explicit HeartbeatFlusher(std::function<int()> drain);
  ~HeartbeatFlusher();
  void start();
  void stop();
private:
  std::function<int()> drain_;
  std::atomic<bool> running_{false};
  std::thread th_;
};

std::string generate_job_id();

class SubmissionQueue {
public:
  explicit SubmissionQueue(std::function<void(const std::string&,const std::string&)> lpush,
                           std::function<std::optional<std::string>(const std::string&,int)> brpop,
                           std::function<void(const std::string&,const std::string&)> set_result);
  std::string enqueue(const std::map<std::string,std::string>& data);
  std::optional<SubmissionJob> dequeue(int timeout_sec=5);
  bool requeue(const SubmissionJob& job);
  void store_result(const JobResult& r);
  // P17-L3: hook LPUSH tercek (dipakai bila di-set; untuk test + Redis nyata
  // yang melaporkan hasil). Bila hook mengembalikan false → requeue gagal.
  void set_lpush_checked(std::function<bool(const std::string&,const std::string&)> fn);
 private:
  std::function<void(const std::string&,const std::string&)> lpush_;
  std::function<bool(const std::string&,const std::string&)> lpush_checked_;
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
  // Batch berisi (job, skor) agar skor bisa dipersist ke tabel submissions.
  std::queue<std::pair<SubmissionJob,std::optional<double>>> batch_q_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
};

} // namespace examvan::queue
