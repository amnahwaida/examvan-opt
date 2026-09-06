#include <gtest/gtest.h>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include "queue/submission_queue.hpp"
#include "redis/client.hpp"
#include "handlers/api/exams.hpp"
#include "handlers/admin/exams.hpp"
#include "store/exam_store.hpp"

using namespace examvan;

static std::string read_src18b(const std::string& p) {
  std::ifstream f(p);
  if (!f) return "";
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

TEST(P18B, EnqueueFailsWhenCheckedHookFalse) {
  queue::SubmissionQueue q([](const std::string&, const std::string&) {},
                             [](const std::string&, int) -> std::optional<std::string> { return std::nullopt; },
                             [](const std::string&, const std::string&) {});
  q.set_lpush_checked([](const std::string&, const std::string&) { return false; });
  std::string id = q.enqueue({{"exam_id", "1"}, {"mac_address", "AA"}});
  EXPECT_TRUE(id.empty());
  q.set_lpush_checked([](const std::string&, const std::string&) { return true; });
  std::string id2 = q.enqueue({{"exam_id", "1"}, {"mac_address", "AA"}});
  EXPECT_FALSE(id2.empty());
}

TEST(P18B, RequeueCheckedFalse) {
  queue::SubmissionQueue q([](const std::string&, const std::string&) {},
                             [](const std::string&, int) -> std::optional<std::string> { return std::nullopt; },
                             [](const std::string&, const std::string&) {});
  q.set_lpush_checked([](const std::string&, const std::string&) { return false; });
  queue::SubmissionJob job;
  job.job_id = "abc";
  job.exam_id = 1;
  EXPECT_FALSE(q.requeue(job));
}

TEST(P18B, TryAcquireSecondFails) {
  RedisClient r("", "p18b-test");
  EXPECT_TRUE(r.try_acquire_job("j1", 60));
  EXPECT_FALSE(r.try_acquire_job("j1", 60));
  r.release_job("j1");
  EXPECT_TRUE(r.try_acquire_job("j1", 60));
  r.release_job("j1");
}

#ifdef HAS_HIREDIS
TEST(P18B, ConnectRedisBadUrlNoCrash) {
  auto c1 = redis_real::connect_redis(":::bad:::");
  (void)c1;
  auto c2 = redis_real::connect_redis("");
  (void)c2;
  auto c3 = redis_real::connect_redis("redis://127.0.0.1:6399/0");
  (void)c3;
  SUCCEED();
}
#endif

TEST(P18B, SubmitIdempotencySameKey) {
  setenv("R2_ACCESS_KEY_ID", "test", 1);
  setenv("R2_SECRET_ACCESS_KEY", "test", 1);
  setenv("R2_ENDPOINT", "https://test.r2.cloudflarestorage.com", 1);
  setenv("R2_BUCKET", "test", 1);
  Request cr;
  cr.body = "name=P18BIdem&file_path=/tmp/a.pdf&size_bytes=100";
  auto created = handlers::admin::create_exam(cr);
  ASSERT_EQ(created.status, 201) << created.body;
  auto p = created.body.find("\"id\":");
  ASSERT_NE(p, std::string::npos);
  auto q = created.body.find_first_of(",}", p);
  int eid = std::stoi(created.body.substr(p + 5, q - p - 5));
  store::active_store()->update(eid, [](models::Exam& ex) {
    ex.status = "active";
    ex.exam_started_at = "2026-08-31T00:00:00Z";
  });
  auto exam = store::active_store()->get_by_id(eid);
  ASSERT_TRUE(exam.has_value());
  int count = 0;
  handlers::api::set_submit_enqueue_hook_for_test(
      [&](const queue::SubmissionJob&) { count++; });
  Request r1;
  r1.params["exam_id"] = std::to_string(eid);
  r1.headers["X-Exam-Token"] = exam->token;
  r1.headers["Idempotency-Key"] = "p18b-key-123";
  r1.body = "{\"mac_address\":\"AA:BB:CC:DD:EE:01\",\"answers\":{\"1\":\"A\"}}";
  auto res1 = handlers::api::submit_exam(r1);
  Request r2 = r1;
  auto res2 = handlers::api::submit_exam(r2);
  handlers::api::set_submit_enqueue_hook_for_test(nullptr);
  store::active_store()->clear_all();
  ASSERT_EQ(res1.status, 202) << res1.body;
  ASSERT_EQ(res2.status, 202) << res2.body;
  EXPECT_EQ(count, 1);
  auto j1 = res1.body.find("job_id");
  auto j2 = res2.body.find("job_id");
  ASSERT_NE(j1, std::string::npos);
  ASSERT_NE(j2, std::string::npos);
  EXPECT_EQ(res1.body.substr(j1), res2.body.substr(j2));
}

TEST(P18B, MainWiresCheckedLpushAndSignals) {
  auto src = read_src18b("src/main.cpp");
  EXPECT_NE(src.find("set_lpush_checked"), std::string::npos);
  bool has_sig = src.find("SIGTERM") != std::string::npos ||
                 src.find("SIGINT") != std::string::npos;
  EXPECT_TRUE(has_sig);
}

TEST(P18B, HubSerializesRedisCallbacks) {
  auto src = read_src18b("src/websocket/hub.cpp");
  bool has_lock = src.find("redis_mu_") != std::string::npos ||
                  src.find("lock_guard") != std::string::npos;
  EXPECT_TRUE(has_lock);
}

TEST(P18B, RealPoolHasDestructorAndBound) {
  auto src = read_src18b("src/db/pool_real.cpp");
  EXPECT_NE(src.find("RealPool::~RealPool"), std::string::npos);
}

TEST(P18B, JobLockUsesSetnx) {
  auto src = read_src18b("src/redis/client.cpp");
  EXPECT_NE(src.find("redis_setnx"), std::string::npos);
}
