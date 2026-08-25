#include "jobs/jobs.hpp"
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
void run_expiry_job(){}
void run_approval_cleanup(){}
void run_access_log_retention(){}
} // namespace examvan::jobs
