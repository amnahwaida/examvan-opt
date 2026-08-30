#include "store/exam_store.hpp"
#include "store/exam_store_memory.hpp"
#include <algorithm>
#include <mutex>

namespace examvan::store {

namespace {
ExamStoreMemory g_memory_store;
ExamStore* g_active_store = nullptr;
std::mutex g_store_swap_mu;
}

ExamStore* memory_store(){ return &g_memory_store; }

void set_active_store(ExamStore* store){
  std::lock_guard<std::mutex> g(g_store_swap_mu);
  g_active_store = store ? store : &g_memory_store;
}

ExamStore* active_store(){
  std::lock_guard<std::mutex> g(g_store_swap_mu);
  return g_active_store ? g_active_store : &g_memory_store;
}

int ExamStoreMemory::next_id(){
  return next_id_.fetch_add(1);
}

bool ExamStoreMemory::add(const models::Exam& e){
  std::lock_guard<std::mutex> g(mu_);
  // Cek token duplikat hanya terhadap exams_[] (bukan seen_tokens_).
  // Token auto-gen sudah di-claim via claim_token() sebelum add();
  // seen_tokens_ hanya dipakai untuk tracking claim/unclaim.
  for(auto& ex: exams_) if(ex.token==e.token) return false;
  exams_.push_back(e);
  return true;
}

std::optional<models::Exam> ExamStoreMemory::get_by_id(int id){
  std::lock_guard<std::mutex> g(mu_);
  for(auto& e: exams_) if(e.id==id) return e;
  return std::nullopt;
}

std::vector<models::Exam> ExamStoreMemory::list_all(){
  std::lock_guard<std::mutex> g(mu_);
  return exams_;
}

bool ExamStoreMemory::token_exists(const std::string& token, int exclude_id){
  std::lock_guard<std::mutex> g(mu_);
  for(auto& e: exams_){
    if(exclude_id>0 && e.id==exclude_id) continue;
    if(e.token==token) return true;
  }
  return false;
}

bool ExamStoreMemory::claim_token(const std::string& token){
  std::lock_guard<std::mutex> g(mu_);
  // Cek seen_tokens_ (auto-gen) DAN exams_[] (custom token yang sudah ada)
  if(seen_tokens_.count(token)) return false;
  for(auto& e: exams_) if(e.token==token) return false;
  seen_tokens_.insert(token);
  return true;
}

void ExamStoreMemory::unclaim_token(const std::string& token){
  std::lock_guard<std::mutex> g(mu_);
  seen_tokens_.erase(token);
}

bool ExamStoreMemory::update(int id, const std::function<void(models::Exam&)>& mutator){
  std::lock_guard<std::mutex> g(mu_);
  for(auto& e: exams_){
    if(e.id==id){ mutator(e); return true; }
  }
  return false;
}

bool ExamStoreMemory::remove(int id){
  std::lock_guard<std::mutex> g(mu_);
  for(auto it=exams_.begin();it!=exams_.end();++it){
    if(it->id==id){
      seen_tokens_.erase(it->token);
      exams_.erase(it);
      return true;
    }
  }
  return false;
}

size_t ExamStoreMemory::count(){
  std::lock_guard<std::mutex> g(mu_);
  return exams_.size();
}

void ExamStoreMemory::clear_all(){
  std::lock_guard<std::mutex> g(mu_);
  exams_.clear();
  seen_tokens_.clear();
  // next_id_ TIDAK di-reset — counter monotonik (Bug 5 fix)
}

} // namespace examvan::store
