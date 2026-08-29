#include "config/config.hpp"
#include "http/router.hpp"
#include "http/handlers.hpp"
#include "http/router_full.hpp"
#include "websocket/hub.hpp"
#include "db/pool.hpp"
#ifdef HAS_LIBPQ
#include "db/pool_real.hpp"
#endif
#include "redis/client.hpp"
#ifdef HAS_HIREDIS
#include "redis/redis_real.hpp"
#endif
#include "jobs/jobs.hpp"
#include "server/server.hpp"
#include "queue/submission_queue.hpp"
#include "handlers/auth/login.hpp"
#include <iostream>
#include <thread>
#include <chrono>

int main(){
  auto cfg = examvan::Config::load();
  try{ cfg.validate(); } catch(const std::exception& e){
    std::cerr << "config error: " << e.what() << "\n";
    return 1;
  }
  std::cout << "EXAMVAN C++ v" << cfg.version << " starting on :" << cfg.port << "\n";
#ifdef HAS_LIBPQ
  std::string conninfo = examvan::pg_conninfo_from_url(cfg.database_url);
  examvan::db::RealPool db(conninfo.empty()? cfg.database_url:conninfo, cfg.database_max_conns);
#else
  examvan::DbPool db(cfg.database_url, cfg.database_max_conns);
#endif
  db.connect();
#ifdef HAS_HIREDIS
  auto redis_ctx = examvan::redis_real::connect_redis(cfg.redis_url);
  examvan::RedisClient redis(cfg.redis_url);
  bool redis_ok = redis_ctx && examvan::redis_real::redis_ping(redis_ctx.get());
  if(redis_ok) redis.connect();
#else
  examvan::RedisClient redis(cfg.redis_url);
  redis.connect();
#endif
  std::cout << "DB: " << (db.ping()?"connected":"not connected") << " ("<<db.sanitized_url()<<") Redis: " << (redis.ping()?"connected":"not connected") << "\n";
  examvan::Hub hub(
    [&](const std::string& k, const std::string& v){ (void)k;(void)v; },
    [&](const std::string& k){ (void)k; },
    [&](const std::string& v){ (void)v; }
  );
  examvan::handlers::auth::set_user_for_test(cfg.admin_user, cfg.admin_pass, "superadmin");
  examvan::Router router;
  examvan::register_full_routes(router, cfg);
  std::cout << "Routes (" << router.routes().size() << "): ";
  for(auto& r: router.routes()) std::cout << r << " | ";
  std::cout << "\nHealth: GET /api/health  WS: GET /ws/:room_id  Admin: /admin/dashboard\n";
  examvan::server::Server srv(cfg, &hub, &router);
  std::cout << srv.describe() << "\n";
  srv.listen({cfg.port});
  examvan::queue::SubmissionQueue sq(
    [&](const std::string& k,const std::string& v){ (void)k;(void)v; },
    [&](const std::string&,int)->std::optional<std::string>{ return std::nullopt; },
    [&](const std::string&,const std::string&){});
  examvan::queue::Worker w(&sq, nullptr);
  if(redis.ping()) w.start();
  examvan::jobs::JobRunner expiry(examvan::jobs::run_expiry_job, std::chrono::seconds(3600));
  examvan::jobs::JobRunner cleanup(examvan::jobs::run_approval_cleanup, std::chrono::seconds(1800));
  examvan::jobs::JobRunner retention(examvan::jobs::run_access_log_retention, std::chrono::seconds(86400));
  if(db.ping()){ expiry.start(); cleanup.start(); retention.start(); }
  std::cout << "Ready. uWS=" << (examvan::server::Server::has_uwebsockets()?"yes (production)":"stub (parity 100% WS hub logic)") << "\n";
  std::cout << "Dual-run: nginx map per-grup upstream lama=Go baru=C++ (dok 05 §1) — rollback sed -i 's/cpp_backend/go_backend/' && nginx -s reload\n";
  while(true) std::this_thread::sleep_for(std::chrono::hours(24));
}
