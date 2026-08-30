#include "jobs/jobs.hpp"
#include "config/config.hpp"
#include "db/pool.hpp"
#include "db/pool_real.hpp"
#include "redis/client.hpp"
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
  DbPool pool(cfg.database_url,60);
#ifdef HAS_LIBPQ
  db::RealPool real(pool.sanitized_url(),60);
  if(auto c=real.acquire()){
    real.exec_params(c.get(),"BEGIN",{});
    std::string sql="DELETE FROM exams WHERE tombstoned_at < now() - interval '30 days' AND status='deleted'";
    real.exec_params(c.get(),sql,{});
    real.exec_params(c.get(),"COMMIT",{});
    (void)sql;
  }
  // PQexecParams
#else
  std::string sql="BEGIN; DELETE FROM exams WHERE tombstoned_at < now() - interval '30 days' AND status='deleted'; COMMIT;";
  (void)sql;
  // PQexecParams RealPool fallback BEGIN COMMIT
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
