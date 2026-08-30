#pragma once
#include "exam_store.hpp"
#include <vector>
#include <mutex>
#include <atomic>
#include <unordered_set>

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
  bool add(const models::Exam& e) override;
  std::optional<models::Exam> get_by_id(int id) override;
  std::vector<models::Exam> list_all() override;
  bool token_exists(const std::string& token, int exclude_id) override;
  bool claim_token(const std::string& token) override;
  void unclaim_token(const std::string& token) override;
  bool update(int id, const std::function<void(models::Exam&)>& mutator) override;
  bool remove(int id) override;
  size_t count() override;
  void clear_all() override;

private:
  mutable std::mutex mu_;
  std::vector<models::Exam> exams_;
  std::atomic<int> next_id_{1};

  // Token collision tracking — O(1) lookup via unordered_set.
  // Semua token (auto-gen DAN custom) tercatat di sini setelah disimpan
  // ke exams_, sehingga claim_token() bisa mengecek keduanya.
  std::unordered_set<std::string> seen_tokens_;
};

} // namespace examvan::store
