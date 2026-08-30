#pragma once
#include "store/exam_store.hpp"

#ifdef HAS_LIBPQ
#include "db/pool_real.hpp"
#include <mutex>

namespace examvan::store {

class ExamStorePostgres final : public ExamStore {
public:
  explicit ExamStorePostgres(db::RealPool& pool): pool_(pool) {}

  bool migrate();
  bool hydrate();
  bool ready() const;

  int next_id() override;
  bool add(const models::Exam& e) override;
  std::optional<models::Exam> get_by_id(int id) override;
  std::vector<models::Exam> list_all() override;
  bool token_exists(const std::string& token, int exclude_id = 0) override;
  bool claim_token(const std::string& token) override;
  void unclaim_token(const std::string& token) override;
  bool update(int id, const std::function<void(models::Exam&)>& mutator) override;
  bool remove(int id) override;
  size_t count() override;
  void clear_all() override;

private:
  bool exec_command(const std::string& sql, const std::vector<std::string>& params = {});
  bool exec_command_nullable(const std::string& sql, const std::vector<std::optional<std::string>>& params);
  std::vector<models::Exam> query_exams(const std::string& sql, const std::vector<std::string>& params = {});
  bool execute_transaction(const std::vector<std::pair<std::string,std::vector<std::string>>>& statements);
  static std::optional<std::string> nullable(PGresult* result, int row, int col);
  static models::Exam map_exam(PGresult* result, int row);
  db::RealPool& pool_;
  mutable std::mutex mu_;
  bool ready_{false};
};

} // namespace examvan::store
#endif
