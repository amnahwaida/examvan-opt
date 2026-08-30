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

void ExamStoreMemory::add(const models::Exam& e){
  std::lock_guard<std::mutex> g(mu_);
  exams_.push_back(e);
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
  auto it=std::find(seen_tokens_.begin(), seen_tokens_.end(), token);
  if(it!=seen_tokens_.end()) return false;
  seen_tokens_.push_back(token);
  return true;
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
  auto before=exams_.size();
  exams_.erase(std::remove_if(exams_.begin(), exams_.end(), [id](const models::Exam& e){ return e.id==id; }), exams_.end());
  return exams_.size()!=before;
}

size_t ExamStoreMemory::count(){
  std::lock_guard<std::mutex> g(mu_);
  return exams_.size();
}

void ExamStoreMemory::clear_all(){
  std::lock_guard<std::mutex> g(mu_);
  exams_.clear();
  seen_tokens_.clear();
  next_id_.store(1);
}

} // namespace examvan::store
