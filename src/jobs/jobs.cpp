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
  //    Exam yang sudah tombstoned dilewati (tidak di-tombstone ulang).
  //    Paritas schema Go: exams.status punya CHECK (active/inactive) —
  //    tombstone = status 'inactive' + tombstoned_at ("virtual status"
  //    tombstoned di dashboard Go), BUKAN 'deleted' (akan ditolak PG).
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
      store->update(e.id,[&](models::Exam& ex){ ex.tombstoned_at=now_text; ex.status="inactive"; });
    }
  }
  // Catatan: TIDAK ada purge exam otomatis. Go (webui) tidak pernah
  // auto-delete exam — penghapusan hanya eksplisit via delete_exam (yang
  // juga membersihkan object R2). Menghapus exam diam-diam bisa memutus
  // riwayat submissions/access_logs yang me-reference exam tsb.
  redis.release_job("expiry");
}
void run_approval_cleanup(){
  auto cfg = examvan::Config::load();
  RedisClient redis(cfg.redis_url);
  if(!redis.try_acquire_job("approval_cleanup",1800)) return;
  DbPool pool(cfg.database_url,60);
#ifdef HAS_LIBPQ
  db::RealPool real(examvan::conninfo_from_url_or_raw(pool.url),60);
  if(auto c=real.acquire()){
    // Paritas Go models.PurgeStaleExamApprovals — tabel = exam_approvals
    // (bukan approvals; tidak ada kolom expires_at di schema Go). Rejected
    // TIDAK pernah disentuh; submissions & access logs juga tidak.
    real.exec_params(c.get(),
      "DELETE FROM exam_approvals a USING exams e WHERE a.exam_id=e.id AND a.status='pending' AND e.end_time IS NOT NULL AND e.end_time < now() - interval '1 hour'",{});
    real.exec_params(c.get(),
      "DELETE FROM exam_approvals a USING exams e WHERE a.exam_id=e.id AND a.status='approved' AND e.end_time IS NOT NULL AND e.end_time < now() - interval '1 hour'",{});
    real.exec_params(c.get(),
      "DELETE FROM exam_approvals a USING exams e WHERE a.exam_id=e.id AND a.status='pending' AND e.status='inactive' AND a.created_at < now() - interval '24 hours'",{});
    real.exec_params(c.get(),
      "DELETE FROM exam_approvals a USING exams e WHERE a.exam_id=e.id AND a.status='approved' AND e.status='inactive' AND a.created_at < now() - interval '24 hours'",{});
  }
#else
  (void)pool;
#endif
  redis.release_job("approval_cleanup");
}
void run_access_log_retention(){
  auto cfg = examvan::Config::load();
  RedisClient redis(cfg.redis_url);
  if(!redis.try_acquire_job("access_log_retention",86400)) return;
  DbPool pool(cfg.database_url,60);
#ifdef HAS_LIBPQ
  db::RealPool real(examvan::conninfo_from_url_or_raw(pool.url),60);
  if(auto c=real.acquire()){
    // Paritas Go PurgeOldStudentAccessLogs — tabel = student_access_logs
    // (bukan access_log). Transaksional via libpq: BEGIN → DELETE → COMMIT.
    real.exec_params(c.get(),"BEGIN",{});
    real.exec_params(c.get(),"DELETE FROM student_access_logs WHERE created_at < now() - interval '90 days'",{});
    real.exec_params(c.get(),"COMMIT",{});
  }
#else
  (void)pool;
#endif
  // PQexecParams
  redis.release_job("access_log_retention");
}
} // namespace examvan::jobs
