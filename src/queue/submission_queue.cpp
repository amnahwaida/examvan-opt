#include "queue/submission_queue.hpp"
#include "helpers/utils.hpp"
#include <chrono>
#include <random>
#include <sstream>

namespace examvan::queue {

std::string generate_job_id(){
  std::random_device rd; std::mt19937 g(rd());
  std::uniform_int_distribution<int> d(0,255);
  std::ostringstream ss; for(int i=0;i<8;i++) ss<< std::hex << std::setw(2) << std::setfill('0') << d(g);
  return ss.str();
}

static std::string json_escape(const std::string& s){
  std::string o; o.reserve(s.size()+8);
  for(unsigned char c: s){
    switch(c){
      case '"': o+="\\\""; break;
      case '\\': o+="\\\\"; break;
      case '\b': o+="\\b"; break;
      case '\f': o+="\\f"; break;
      case '\n': o+="\\n"; break;
      case '\r': o+="\\r"; break;
      case '\t': o+="\\t"; break;
      default:
        if(c<0x20){ char buf[7]; snprintf(buf,sizeof(buf),"\\u%04x",c); o+=buf; }
        else o+=char(c);
    }
  }
  return o;
}
static std::string json_unescape(const std::string& s){
  std::string o; o.reserve(s.size());
  for(size_t i=0;i<s.size();){
    if(s[i]=='\\' && i+1<s.size()){
      char n=s[i+1];
      if(n=='"') o+='"';
      else if(n=='\\') o+='\\';
      else if(n=='n') o+='\n';
      else if(n=='r') o+='\r';
      else if(n=='t') o+='\t';
      else if(n=='b') o+='\b';
      else if(n=='f') o+='\f';
      else if(n=='u' && i+5<s.size()){ int v=0; for(int k=2;k<6;k++){ char h=s[i+k]; v*=16; if(h>='0'&&h<='9') v+=h-'0'; else if(h>='a'&&h<='f') v+=h-'a'+10; else if(h>='A'&&h<='F') v+=h-'A'+10; } o+=char(v); i+=6; continue; }
      else o+=n;
      i+=2;
    } else { o+=s[i]; i++; }
  }
  return o;
}
std::string SubmissionJob::to_json() const {
  std::ostringstream ss;
  ss<<"{\"job_id\":\""<<json_escape(job_id)<<"\",\"exam_id\":"<<exam_id
    <<",\"student_name\":\""<<json_escape(student_name)<<"\",\"exam_number\":\""<<json_escape(exam_number)
    <<"\",\"student_class\":\""<<json_escape(student_class)<<"\",\"mac_address\":\""<<json_escape(mac_address)
    <<"\",\"retries\":"<<retries<<",\"enqueued_at\":\""<<json_escape(enqueued_at)<<"\"}";
  return ss.str();
}

std::optional<SubmissionJob> SubmissionJob::from_json(const std::string& s){
  SubmissionJob j;
  auto extract=[&](const std::string& key)->std::string{
    std::string needle="\""+key+"\"";
    auto p=s.find(needle); if(p==std::string::npos) return "";
    auto c=s.find(':',p); if(c==std::string::npos) return "";
    size_t q1=s.find_first_not_of(" \t",c+1);
    if(q1==std::string::npos) return "";
    if(s[q1]=='"'){
      size_t q2=q1+1;
      while(q2<s.size()){
        if(s[q2]=='\\'){ q2+=2; continue; }
        if(s[q2]=='"') break;
        q2++;
      }
      if(q2>=s.size()) return "";
      return json_unescape(s.substr(q1+1,q2-q1-1));
    }
    size_t q2=s.find_first_of(",}",q1);
    if(q2==std::string::npos) return "";
    std::string v=s.substr(q1,q2-q1);
    v.erase(std::remove(v.begin(),v.end(),' '),v.end());
    return v;
  };
  j.job_id=extract("job_id");
  if(j.job_id.empty()) return std::nullopt;
  try{j.exam_id=std::stoi(extract("exam_id"));}catch(...){}
  j.student_name=extract("student_name");
  j.mac_address=extract("mac_address");
  return j;
}

std::string JobResult::to_json() const {
  std::ostringstream ss;
  ss<<"{\"job_id\":\""<<job_id<<"\",\"success\":"<<(success?"true":"false")
    <<",\"message\":\""<<message<<"\",\"processed_at\":\""<<processed_at<<"\"}";
  return ss.str();
}

SubmissionQueue::SubmissionQueue(std::function<void(const std::string&,const std::string&)> lpush,
                                 std::function<std::optional<std::string>(const std::string&,int)> brpop,
                                 std::function<void(const std::string&,const std::string&)> set_result)
  : lpush_(std::move(lpush)), brpop_(std::move(brpop)), set_(std::move(set_result)) {}

std::string SubmissionQueue::enqueue(const std::map<std::string,std::string>& data){
  SubmissionJob j;
  j.job_id=generate_job_id();
  auto it=data.find("exam_id"); if(it!=data.end()) try{j.exam_id=std::stoi(it->second);}catch(...){}
  it=data.find("student_name"); if(it!=data.end()) j.student_name=it->second;
  it=data.find("exam_number"); if(it!=data.end()) j.exam_number=it->second;
  it=data.find("student_class"); if(it!=data.end()) j.student_class=it->second;
  it=data.find("mac_address"); if(it!=data.end()) j.mac_address=it->second;
  j.enqueued_at=helpers::format_iso_utc(std::chrono::system_clock::now());
  std::string payload=j.to_json();
  if(lpush_) lpush_(kQueueKey, payload);
  return j.job_id;
}

std::optional<SubmissionJob> SubmissionQueue::dequeue(int timeout){
  if(!brpop_) return std::nullopt;
  auto raw=brpop_(kQueueKey, timeout);
  if(!raw) return std::nullopt;
  return SubmissionJob::from_json(*raw);
}

void SubmissionQueue::store_result(const JobResult& r){
  if(set_) set_(std::string(kResultKeyPrefix)+r.job_id, r.to_json());
}

Worker::Worker(SubmissionQueue* q, std::function<std::optional<double>(const SubmissionJob&)> scorer): queue_(q), scorer_(std::move(scorer)) {}

void Worker::start(){
  running_=true;
  for(int i=0;i<kWorkerCount;i++) workers_.emplace_back(&Worker::run_worker,this,i);
  batch_th_=std::thread(&Worker::run_batch,this);
}

void Worker::stop(){
  running_=false;
  cv_.notify_all();
  for(auto& t: workers_) if(t.joinable()) t.join();
  if(batch_th_.joinable()) batch_th_.join();
}

size_t Worker::pending() const { std::lock_guard<std::mutex> g(mu_); return batch_q_.size(); }

void Worker::run_worker(int id){
  (void)id;
  while(running_){
    auto job=queue_->dequeue(5);
    if(!job) continue;
    std::optional<double> score;
    if(scorer_) score=scorer_(*job);
    {
      std::lock_guard<std::mutex> g(mu_);
      batch_q_.push(*job);
    }
    cv_.notify_one();
    JobResult r{job->job_id, true, score, "ok", helpers::format_iso_utc(std::chrono::system_clock::now())};
    queue_->store_result(r);
    if(batch_q_.size()>=kBatchSize) cv_.notify_one();
  }
}

void Worker::run_batch(){
  while(running_){
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait_for(lk, std::chrono::seconds(5), [this]{ return !batch_q_.empty() || !running_; });
    std::vector<SubmissionJob> batch;
    while(!batch_q_.empty()){
      batch.push_back(batch_q_.front());
      batch_q_.pop();
    }
    lk.unlock();
    if(!batch.empty()){
#ifdef HAS_LIBPQ
      (void)batch;
#else
      (void)batch;
#endif
    }
  }
}

} // namespace examvan::queue
