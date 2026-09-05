#include "queue/submission_queue.hpp"
#include "helpers/utils.hpp"
#include "config/config.hpp"
#include "db/pool.hpp"
#include "db/pool_real.hpp"
#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>
#include <openssl/rand.h>
#ifdef HAS_PROTOBUF
#include "examvan.pb.h"
#endif
#ifdef HAS_HIREDIS
#include "redis/redis_real.hpp"
#include <hiredis/hiredis.h>
#endif
#ifdef HAS_LIBPQ
#include <libpq-fe.h>
#endif

namespace examvan::queue {

std::string generate_job_id(){
  unsigned char buf[8];
  if(RAND_bytes(buf,sizeof(buf))==1){
    std::ostringstream ss; ss<< std::hex << std::setfill('0');
    for(int i=0;i<8;i++) ss<< std::setw(2) << (int)buf[i];
    return ss.str();
  }
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
      else if(n=='u' && i+5<s.size()){
        int v=0; bool ok=true;
        for(int k=2;k<6;k++){ char h=s[i+k]; v*=16; if(h>='0'&&h<='9') v+=h-'0'; else if(h>='a'&&h<='f') v+=h-'a'+10; else if(h>='A'&&h<='F') v+=h-'A'+10; else ok=false; }
        if(ok){
          if(v<=0x7F) o+=char(v);
          else if(v<=0x7FF){ o+=char(0xC0|(v>>6)); o+=char(0x80|(v&0x3F)); }
          else { o+=char(0xE0|(v>>12)); o+=char(0x80|((v>>6)&0x3F)); o+=char(0x80|(v&0x3F)); }
          i+=6; continue;
        } else o+=n;
      }
      else o+=n;
      i+=2;
    } else { o+=s[i]; i++; }
  }
  return o;
}
// Parse {"1":"A","2":"B"} di dalam objek JSON untuk key tertentu.
static std::map<std::string,std::string> parse_map_from_json(const std::string& s, const std::string& key){
  std::map<std::string,std::string> out;
  std::string needle="\""+key+"\"";
  auto p=s.find(needle); if(p==std::string::npos) return out;
  auto c=s.find(':',p); if(c==std::string::npos) return out;
  size_t q=s.find('{',c); if(q==std::string::npos) return out;
  size_t e=q; int depth=0; bool in_str=false; bool esc=false;
  for(;e<s.size();++e){
    char ch=s[e];
    if(esc){ esc=false; continue; }
    if(ch=='\\' && in_str){ esc=true; continue; }
    if(ch=='"'){ in_str=!in_str; continue; }
    if(in_str) continue;
    if(ch=='{') depth++;
    else if(ch=='}'){ depth--; if(depth==0) break; }
  }
  if(e>=s.size() || depth!=0) return out;
  std::string obj=s.substr(q,e-q+1);
  size_t i=1;
  while(i<obj.size()){
    while(i<obj.size() && (obj[i]==' '||obj[i]=='\t'||obj[i]=='\n'||obj[i]=='\r'||obj[i]==',')) i++;
    if(i>=obj.size() || obj[i]=='}') break;
    if(obj[i]!='"'){ i++; continue; }
    size_t k1=i+1; size_t k2=k1;
    while(k2<obj.size()){ if(obj[k2]=='\\'){ k2+=2; continue; } if(obj[k2]=='"') break; k2++; }
    if(k2>=obj.size()) break;
    std::string k=json_unescape(obj.substr(k1,k2-k1));
    i=k2+1;
    while(i<obj.size() && (obj[i]==' '||obj[i]=='\t'||obj[i]=='\n'||obj[i]=='\r'||obj[i]==':')) i++;
    if(i>=obj.size() || obj[i]!='"'){ i++; continue; }
    size_t v1=i+1; size_t v2=v1;
    while(v2<obj.size()){ if(obj[v2]=='\\'){ v2+=2; continue; } if(obj[v2]=='"') break; v2++; }
    if(v2>=obj.size()) break;
    out[k]=json_unescape(obj.substr(v1,v2-v1));
    i=v2+1;
  }
  return out;
}
static std::string map_to_json(const std::map<std::string,std::string>& m){
  std::ostringstream ss; ss<<"{";
  bool first=true;
  for(auto& kv: m){
    if(!first) ss<<",";
    first=false;
    ss<<"\""<<json_escape(kv.first)<<"\":\""<<json_escape(kv.second)<<"\"";
  }
  ss<<"}";
  return ss.str();
}
std::string SubmissionJob::to_json() const {
  std::ostringstream ss;
  ss<<"{\"job_id\":\""<<json_escape(job_id)<<"\",\"exam_id\":"<<exam_id
    <<",\"student_name\":\""<<json_escape(student_name)<<"\",\"exam_number\":\""<<json_escape(exam_number)
    <<"\",\"student_class\":\""<<json_escape(student_class)<<"\",\"mac_address\":\""<<json_escape(mac_address)<<"\",\"start_time\":\""<<json_escape(start_time)
    <<"\",\"retries\":"<<retries<<",\"enqueued_at\":\""<<json_escape(enqueued_at)<<"\""
    <<",\"answers\":"<<map_to_json(answers)
    <<",\"identity_data\":"<<map_to_json(identity_data)
    <<"}";
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
  j.exam_number=extract("exam_number");
  j.student_class=extract("student_class");
  j.start_time=extract("start_time");
  j.mac_address=extract("mac_address");
  j.enqueued_at=extract("enqueued_at");
  try{j.retries=std::stoi(extract("retries"));}catch(...){}
  j.answers=parse_map_from_json(s,"answers");
  j.identity_data=parse_map_from_json(s,"identity_data");
  return j;
}

#ifdef HAS_PROTOBUF
std::string SubmissionJob::to_protobuf() const {
  examvan::v1::SubmissionJob pb;
  pb.set_job_id(job_id);
  pb.set_exam_id(exam_id);
  pb.set_student_name(student_name);
  pb.set_exam_number(exam_number);
  pb.set_student_class(student_class);
  pb.set_mac_address(mac_address);
  pb.set_start_time(start_time);
  pb.set_retries(retries);
  pb.set_enqueued_at(enqueued_at);
  for(const auto& kv: answers) (*pb.mutable_answers())[kv.first]=kv.second;
  for(const auto& kv: identity_data) (*pb.mutable_identity_data())[kv.first]=kv.second;
  std::string out;
  pb.SerializeToString(&out);
  return out;
}
std::optional<SubmissionJob> SubmissionJob::from_protobuf(const std::string& s){
  examvan::v1::SubmissionJob pb;
  if(!pb.ParseFromArray(s.data(), s.size())) return std::nullopt;
  SubmissionJob j;
  j.job_id=pb.job_id();
  j.exam_id=pb.exam_id();
  j.student_name=pb.student_name();
  j.exam_number=pb.exam_number();
  j.student_class=pb.student_class();
  j.mac_address=pb.mac_address();
  j.start_time=pb.start_time();
  j.retries=pb.retries();
  j.enqueued_at=pb.enqueued_at();
  for(const auto& kv: pb.answers()) j.answers[kv.first]=kv.second;
  for(const auto& kv: pb.identity_data()) j.identity_data[kv.first]=kv.second;
  return j;
}
#endif
std::string JobResult::to_json() const {
  std::ostringstream ss;
  ss<<"{\"job_id\":\""<<job_id<<"\",\"success\":"<<(success?"true":"false")
    <<",\"message\":\""<<message<<"\",\"processed_at\":\""<<processed_at<<"\"";
  ss<<",\"score\":";
  if(score.has_value()) ss<<*score; else ss<<"null";
  ss<<"}";
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
#ifdef HAS_PROTOBUF
  auto cfg=Config::load();
  std::string payload = cfg.protobuf_mandatory ? j.to_protobuf() : j.to_json();
  // Fallback to JSON if protobuf not available
  if(payload.empty()) payload=j.to_json();
#else
  std::string payload=j.to_json();
#endif
  if(lpush_) lpush_(kQueueKey, payload);
  return j.job_id;
}

std::optional<SubmissionJob> SubmissionQueue::dequeue(int timeout){
  if(!brpop_) return std::nullopt;
  auto raw=brpop_(kQueueKey, timeout);
  if(!raw) return std::nullopt;
#ifdef HAS_PROTOBUF
  // Try protobuf first, then JSON fallback for dual-support period
  if(auto pb = SubmissionJob::from_protobuf(*raw)) return pb;
#endif
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
      batch_q_.push({*job, score});
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
    std::vector<std::pair<SubmissionJob,std::optional<double>>> batch;
    while(!batch_q_.empty()){
      batch.push_back(batch_q_.front());
      batch_q_.pop();
    }
    lk.unlock();
    if(!batch.empty()){
#ifdef HAS_LIBPQ
      auto cfg = examvan::Config::load();
      examvan::DbPool pool(cfg.database_url, 10);
      // conninfo_from_url_or_raw (BUKAN sanitized_url — password "***" gagal auth).
      examvan::db::RealPool real(examvan::conninfo_from_url_or_raw(pool.url), 10);
      if(auto c=real.acquire()){
        for(auto &b: batch){
          auto& j=b.first;
          auto& score=b.second;
          // M3 (paritas Go upsertSubmissionRow): skema submissions TIDAK punya
          // unique constraint, jadi INSERT ... ON CONFLICT DO NOTHING tidak
          // pernah menahan apa pun → worker selalu INSERT baris baru → siswa
          // yang heartbeat-join (placeholder) LALU submit punya 2 baris, dan
          // retry submit membuat baris ketiga. Upsert: kunci advisory lock per
          // (exam, device), UPDATE baris terakhir utk (exam, device,
          // exam_number bila ada) — placeholder ATAU sudah-submit — baru INSERT
          // bila tak ada baris yang cocok. Retry menimpa baris yang sama.
          std::string score_text = score.has_value() ? std::to_string(*score) : "";
          real.exec_params(c.get(),
            "SELECT pg_advisory_xact_lock(hashtext($1)::bigint)",
            {"approval:"+std::to_string(j.exam_id)+":"+j.mac_address});
          auto up=real.exec_params(c.get(),
            "UPDATE submissions SET answers_json=$1, score=NULLIF($2,'')::double precision,"
            " start_time=COALESCE(start_time,NULLIF($3,'')), student_name=$4, exam_number=$5,"
            " student_class=$6, identity_data=$7"
            " WHERE id=(SELECT id FROM submissions WHERE exam_id=$8 AND mac_address=$9"
            "   AND ($10='' OR exam_number=$10) ORDER BY created_at DESC LIMIT 1)"
            " RETURNING id",
            {map_to_json(j.answers), score_text, j.start_time, j.student_name, j.exam_number,
             j.student_class, map_to_json(j.identity_data), std::to_string(j.exam_id), j.mac_address,
             j.exam_number});
          bool updated=up && PQresultStatus(up.get())==PGRES_TUPLES_OK && PQntuples(up.get())>0;
          if(!updated){
            real.exec_params(c.get(),
              "INSERT INTO submissions (exam_id, student_name, exam_number, student_class, answers_json, score, start_time, mac_address, identity_data)"
              " VALUES ($1,$2,$3,$4,$5,NULLIF($6,'')::double precision,$7,$8,$9)",
              {std::to_string(j.exam_id), j.student_name, j.exam_number, j.student_class,
               map_to_json(j.answers), score_text, j.start_time, j.mac_address, map_to_json(j.identity_data)});
          }
        }
      }
#else
      (void)batch;
#endif
    }
  }
}

namespace {
constexpr int kHeartbeatBatchSize = 500;  // paritas Go heartbeatFlushBatchSize
constexpr int kHeartbeatMaxBatches = 20;  // paritas Go heartbeatFlushMaxBatches

// Ekstrak satu nilai string/angka sederhana dari JSON payload heartbeat.
std::string hb_extract(const std::string& s, const std::string& key){
  std::string needle="\""+key+"\"";
  auto p=s.find(needle); if(p==std::string::npos) return "";
  auto c=s.find(':',p); if(c==std::string::npos) return "";
  size_t q1=s.find_first_not_of(" \t",c+1);
  if(q1==std::string::npos) return "";
  if(s[q1]=='"'){
    size_t q2=q1+1;
    while(q2<s.size()){ if(s[q2]=='\\'){ q2+=2; continue; } if(s[q2]=='"') break; q2++; }
    if(q2>=s.size()) return "";
    return json_unescape(s.substr(q1+1,q2-q1-1));
  }
  size_t q2=s.find_first_of(",}",q1);
  if(q2==std::string::npos) return "";
  std::string v=s.substr(q1,q2-q1);
  v.erase(std::remove(v.begin(),v.end(),' '),v.end());
  return v;
}
} // namespace

std::optional<HeartbeatPayload> parse_heartbeat_payload(const std::string& json){
  if(json.empty()) return std::nullopt;
  const std::string exam_id_s=hb_extract(json,"exam_id");
  if(exam_id_s.empty()) return std::nullopt;
  HeartbeatPayload hb;
  try{ hb.exam_id=std::stoi(exam_id_s); }catch(...){ return std::nullopt; }
  hb.mac_address=hb_extract(json,"mac_address");
  hb.student_name=hb_extract(json,"student_name");
  hb.exam_number=hb_extract(json,"exam_number");
  hb.student_class=hb_extract(json,"student_class");
  hb.device_info=hb_extract(json,"device_info");
  hb.ip_address=hb_extract(json,"ip_address");
  hb.event=hb_extract(json,"event");
  hb.last_seen=hb_extract(json,"last_seen");
  return hb;
}

#ifdef HAS_HIREDIS
#ifdef HAS_LIBPQ
// Pindahkan satu batch heartbeat (max 500) dari queue Redis ke PG dalam SATU
// transaksi; gagal begin/commit → requeue seluruh batch (paritas Go
// flushHeartbeatBatch). Payload rusak di-drop (poison-loop guard).
static int drain_heartbeat_batch(redisContext* ctx, db::RealPool& real, PGconn* conn){
  std::vector<std::string> payloads;
  for(int i=0;i<kHeartbeatBatchSize;i++){
    auto* r=(redisReply*)redisCommand(ctx,"RPOP %s", kHeartbeatQueueKey);
    if(!r) break;
    const bool got = r->type==REDIS_REPLY_STRING;
    if(got) payloads.push_back(std::string(r->str, r->len));
    freeReplyObject(r);
    if(!got) break; // list kosong / error sementara
  }
  if(payloads.empty()) return 0;
  auto requeue=[&]{
    for(auto& p: payloads){
      auto* r=(redisReply*)redisCommand(ctx,"LPUSH %s %b", kHeartbeatQueueKey, p.data(), p.size());
      if(r) freeReplyObject(r);
    }
  };
  real.exec_params(conn,"BEGIN",{});
  for(auto& p: payloads){
    auto hb=parse_heartbeat_payload(p);
    if(!hb) continue; // malformed → drop
    // M2: SAVEPOINT per payload — payload well-formed dengan FK exam yang sudah
    // dihapus membuat statement gagal → tanpa savepoint seluruh transaksi
    // masuk status aborted, COMMIT gagal, dan SELURUH batch 500 di-requeue tiap
    // 30 detik selamanya (poison-loop). Dengan savepoint, satu payload buruk
    // di-rollback dan di-drop; batch lain tetap lanjut.
    real.exec_params(conn,"SAVEPOINT hb_row",{});
    bool row_ok=true;
    const std::string now_txt=helpers::format_iso_utc(std::chrono::system_clock::now());
    auto ins=real.exec_params(conn,
      "INSERT INTO student_access_logs (exam_id, student_identifier, student_name, exam_number, student_class, event, ip_address, device_info, created_at)"
      " VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9)",
      {std::to_string(hb->exam_id), hb->mac_address, hb->student_name, hb->exam_number,
       hb->student_class, hb->event.empty()?"heartbeat":hb->event, hb->ip_address, hb->device_info,
       hb->last_seen.empty()?now_txt:hb->last_seen});
    if(!ins || (PQresultStatus(ins.get())!=PGRES_COMMAND_OK && PQresultStatus(ins.get())!=PGRES_TUPLES_OK)) row_ok=false;
    if(row_ok){
      // Monitoring row (paritas Go): student harus muncul di "Monitoring Perangkat".
      auto latest=real.exec_params(conn,
        "SELECT answers_json FROM submissions WHERE exam_id=$1 AND mac_address=$2 ORDER BY created_at DESC LIMIT 1",
        {std::to_string(hb->exam_id), hb->mac_address});
      bool need_empty=true;
      if(latest && PQresultStatus(latest.get())==PGRES_TUPLES_OK && PQntuples(latest.get())>0){
        const std::string answers=PQgetisnull(latest.get(),0,0)? "": std::string(PQgetvalue(latest.get(),0,0));
        if(answers.empty()) need_empty=false; // placeholder row sudah ada (belum submit)
      }
      if(need_empty){
        auto ph=real.exec_params(conn,
          "INSERT INTO submissions (exam_id, student_name, exam_number, student_class, mac_address, start_time, created_at, identity_data)"
          " VALUES ($1,$2,$3,$4,$5,$6,$7,'{}')",
          {std::to_string(hb->exam_id), hb->student_name, hb->exam_number, hb->student_class,
           hb->mac_address, hb->last_seen.empty()?now_txt:hb->last_seen, now_txt});
        if(!ph || (PQresultStatus(ph.get())!=PGRES_COMMAND_OK && PQresultStatus(ph.get())!=PGRES_TUPLES_OK)) row_ok=false;
      }
    }
    if(!row_ok){
      // Rollback hanya baris ini (mis. FK exam hilang) → drop payload, batch
      // lain tetap diproses. RELEASE setelah rollback membersihkan savepoint.
      real.exec_params(conn,"ROLLBACK TO SAVEPOINT hb_row",{});
      real.exec_params(conn,"RELEASE SAVEPOINT hb_row",{});
    } else {
      real.exec_params(conn,"RELEASE SAVEPOINT hb_row",{});
    }
  }
  auto commit=real.exec_params(conn,"COMMIT",{});
  if(!commit || PQresultStatus(commit.get())!=PGRES_COMMAND_OK){ requeue(); return (int)payloads.size(); }
  return (int)payloads.size();
}
#endif
#endif

int drain_heartbeats_once(){
#ifdef HAS_HIREDIS
#ifdef HAS_LIBPQ
  auto cfg=Config::load();
  auto ctx=examvan::redis_real::connect_redis(cfg.redis_url);
  if(!ctx) return 0;
  DbPool pool(cfg.database_url,60);
  db::RealPool real(examvan::conninfo_from_url_or_raw(pool.url),60);
  auto c=real.acquire();
  if(!c) return 0;
  int total=0;
  for(int batch=0; batch<kHeartbeatMaxBatches; ++batch){
    int n=drain_heartbeat_batch(ctx.get(), real, c.get());
    if(n==0) break;                 // antrean kosong
    total+=n;
    if(n<kHeartbeatBatchSize) break; // batch parsial = antrean habis
  }
  return total;
#else
  return 0;
#endif
#else
  return 0;
#endif
}

HeartbeatFlusher::HeartbeatFlusher(std::function<int()> drain): drain_(std::move(drain)) {}
HeartbeatFlusher::~HeartbeatFlusher(){ stop(); }
void HeartbeatFlusher::start(){
  running_=true;
  th_=std::thread([this]{
    while(running_){
      std::this_thread::sleep_for(std::chrono::seconds(30)); // paritas Go tick 30s
      if(running_ && drain_) drain_();
    }
  });
}
void HeartbeatFlusher::stop(){ running_=false; if(th_.joinable()) th_.join(); }

} // namespace examvan::queue
