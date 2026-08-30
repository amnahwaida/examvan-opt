#pragma once
#include "exam_store.hpp"
#include <vector>
#include <mutex>
#include <atomic>

namespace examvan::store {

/*
 * In-memory ExamStore — production-ready untuk fase sebelum PostgreSQL.
 * Semua akses di-mutex, thread-safe.
 */
class ExamStoreMemory final : public ExamStore {
public:
  ExamStoreMemory() = default;
  ExamStoreMemory(const ExamStoreMemory&) = delete;
  ExamStoreMemory& operator=(const ExamStoreMemory&) = delete;

  int next_id() override;
  void add(const models::Exam& e) override;
  std::optional<models::Exam> get_by_id(int id) override;
  std::vector<models::Exam> list_all() override;
  bool token_exists(const std::string& token, int exclude_id) override;
  bool claim_token(const std::string& token) override;
  bool update(int id, const std::function<void(models::Exam&)>& mutator) override;
  bool remove(int id) override;
  size_t count() override;
  void clear_all() override;

private:
  mutable std::mutex mu_;
  std::vector<models::Exam> exams_;
  std::atomic<int> next_id_{1};

  // Token collision tracking — set token yang sudah pernah dikeluarkan
  // (auto-generated token). Untuk custom token, collision check via
  // token_exists() terhadap exams_.
  std::vector<std::string> seen_tokens_;
};

} // namespace examvan::store
