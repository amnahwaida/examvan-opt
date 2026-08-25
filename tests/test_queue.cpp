#include <gtest/gtest.h>
#include "queue/submission_queue.hpp"
using namespace examvan::queue;

TEST(Queue, EnqueueDequeue) {
  std::string captured_key, captured_val;
  bool set_called=false;
  SubmissionQueue q(
    [&](const std::string& k, const std::string& v){ captured_key=k; captured_val=v; },
    [&](const std::string&,int)->std::optional<std::string>{ return std::nullopt; },
    [&](const std::string&,const std::string&){ set_called=true; }
  );
  std::map<std::string,std::string> data{{"exam_id","5"},{"student_name","Budi"},{"mac_address","aa:bb"}};
  auto jid=q.enqueue(data);
  EXPECT_FALSE(jid.empty());
  EXPECT_EQ(captured_key, std::string(kQueueKey));
  EXPECT_NE(captured_val.find("Budi"), std::string::npos);
}

TEST(Queue, JobJsonRoundTrip) {
  SubmissionJob j; j.job_id="abc123"; j.exam_id=7; j.student_name="Ani"; j.mac_address="11:22";
  auto s=j.to_json();
  auto j2=SubmissionJob::from_json(s);
  ASSERT_TRUE(j2.has_value());
  EXPECT_EQ(j2->job_id, "abc123");
  EXPECT_EQ(j2->exam_id, 7);
}

TEST(Queue, WorkerPending) {
  SubmissionQueue q(nullptr,nullptr,nullptr);
  Worker w(&q, nullptr);
  EXPECT_EQ(w.pending(), 0u);
}

TEST(Queue, GenerateIdUnique) {
  auto a=generate_job_id(); auto b=generate_job_id();
  EXPECT_NE(a,b);
  EXPECT_EQ(a.size(), 16u);
}
