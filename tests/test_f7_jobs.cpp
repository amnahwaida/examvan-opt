#include <gtest/gtest.h>
#include "jobs/jobs.hpp"
#include "redis/client.hpp"
using namespace examvan::jobs;
using namespace examvan;

TEST(F7Jobs, ExpiryJobSETNX) {
  RedisClient redis("redis://redis:6379/0");
  redis.connect();
  EXPECT_TRUE(redis.try_acquire_job("expiry", 60));
  EXPECT_FALSE(redis.try_acquire_job("expiry", 60));
  redis.release_job("expiry");
  EXPECT_TRUE(redis.try_acquire_job("expiry", 60));
  redis.release_job("expiry");
}

TEST(F7Jobs, JobRunnerStartStop) {
  bool ran=false;
  JobRunner jr([&]{ ran=true; }, std::chrono::seconds(1));
  jr.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  jr.stop();
  EXPECT_TRUE(ran);
}

TEST(F7Jobs, QueuePrefixIsolation) {
  RedisClient r1("redis://redis:6379/0","test.shadow");
  EXPECT_EQ(r1.prefixed("examvan:submissions:pending"), "test.shadow:examvan:submissions:pending");
}
