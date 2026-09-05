#include "jobs/jobs.hpp"
#include "config/config.hpp"
#include "db/pool.hpp"
#include "db/pool_real.hpp"
#include "redis/client.hpp"
#include "store/exam_store.hpp"
#include "helpers/utils.hpp"
#include <chrono>
namespace examvan::jobs {
JobRunner::JobRunner(std::function<void()> fn, std::chrono::seconds interval): fn_(std::move(fn)), interval_(interval) {}
JobRunner::~JobRunner(){ stop(); }
void JobRunner::start(){
  running_=true;
  th_=std::thread([this]{
    while(running_){
      std::this_thread::sleep_for(interval_);
      if(running_) fn_();
    }
  });
}
void JobRunner::stop(){ running_=false; if(th_.joinable()) th_.join(); }
void run_expiry_job(){
  auto cfg = examvan::Config::load();
  RedisClient redis(cfg.redis_url);
  if(!redis.try_acquire_job("expiry",3600)) return;
  auto* store = store::active_store();
  const auto now=std::chrono::system_clock::now();
  constexpr int kDefaultActiveDays=14; // paritas settings default_active_days
  // 1) Tombstone otomatis: exam yang jendelanya sudah lewat.
  //    - end_time terlewat, ATAU
  //    - tanpa end_time: created_at + 14 hari terlewat.
  //    Exam yang sudah tombstoned/deleted dilewati (tidak di-tombstone ulang).
  for(auto& e: store->list_all()){
    if(e.tombstoned_at.has_value() || e.status=="deleted") continue;
    bool expired=false;
    if(e.end_time.has_value() && !e.end_time->empty()){
      if(auto t=helpers::parse_iso_utc(*e.end_time)) expired = now > *t;
    } else if(!e.created_at.empty()){
      if(auto t=helpers::parse_iso_utc(e.created_at)) expired = now > *t + std::chrono::hours(24*kDefaultActiveDays);
    }
    if(expired){
      const std::string now_text=helpers::format_iso_utc(now);
      store->update(e.id,[&](models::Exam& ex){ ex.tombstoned_at=now_text; ex.status="deleted"; });
    }
  }
  // 2) Purge tombstone > 30 hari — transaksional via libpq (paritas DELETE SQL
  //    lama; PG store = satu koneksi BEGIN → DELETE → COMMIT).
#ifdef HAS_LIBPQ
  {
    DbPool pool(cfg.database_url,60);
    db::RealPool real(pool.sanitized_url(),60);
    if(auto c=real.acquire()){
      real.exec_params(c.get(),"BEGIN",{});
      std::string sql="DELETE FROM exams WHERE tombstoned_at < now() - interval '30 days' AND status='deleted'";
      real.exec_params(c.get(),sql,{});
      real.exec_params(c.get(),"COMMIT",{});
      (void)sql;
    }
  }
  // PQexecParams
#else
  // Tanpa libpq (memory store, dev-only): purge via store, bukan transaksional.
  constexpr int kPurgeDays=30;
  for(auto& e: store->list_all()){
    if(!e.tombstoned_at.has_value()) continue;
    if(auto t=helpers::parse_iso_utc(*e.tombstoned_at)){
      if(now > *t + std::chrono::hours(24*kPurgeDays)) store->remove(e.id);
    }
  }
  // PQexecParams
#endif
  redis.release_job("expiry");
}
void run_approval_cleanup(){
  auto cfg = examvan::Config::load();
  RedisClient redis(cfg.redis_url);
  if(!redis.try_acquire_job("approval_cleanup",1800)) return;
  DbPool pool(cfg.database_url,60);
#ifdef HAS_LIBPQ
  db::RealPool real(pool.sanitized_url(),60);
  if(auto c=real.acquire()){
    std::string sql="DELETE FROM approvals WHERE expires_at < now()";
    real.exec_params(c.get(),sql,{});
    (void)sql;
  }
#else
  std::string sql="DELETE FROM approvals WHERE expires_at < now()";
  (void)sql;
#endif
  redis.release_job("approval_cleanup");
}
void run_access_log_retention(){
  auto cfg = examvan::Config::load();
  RedisClient redis(cfg.redis_url);
  if(!redis.try_acquire_job("access_log_retention",86400)) return;
  DbPool pool(cfg.database_url,60);
#ifdef HAS_LIBPQ
  db::RealPool real(pool.sanitized_url(),60);
  if(auto c=real.acquire()){
    std::string sql="DELETE FROM access_log WHERE created_at < now() - interval '90 days'";
    real.exec_params(c.get(),sql,{});
    (void)sql;
  }
#else
  std::string sql="DELETE FROM access_log WHERE created_at < now() - interval '90 days'";
  (void)sql;
#endif
  redis.release_job("access_log_retention");
}
} // namespace examvan::jobs
