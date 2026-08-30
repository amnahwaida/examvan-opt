#pragma once
#include "http/router.hpp"
#include "models/exam.hpp"
#include <functional>
#include <string>
namespace examvan::handlers::admin {

using UploadMock = std::function<bool(const std::string& key, const std::string& data)>;

Response list_admin_exams(const Request& req);
Response create_exam(const Request& req);
Response update_exam(const Request& req);
Response delete_exam(const Request& req);
Response export_xlsx(const Request& req);

/* Test-only hooks (dipanggil dari test TDD; tidak dipakai di produksi).
 * Tutorial: g_exams statis di module-level, test perlu reset antar kasus. */
void clear_exams_for_testing();
void set_upload_mock_for_test(std::function<void(const std::string&,const std::string&)> mock);
void set_token_generator_for_test(std::function<std::string(int)> gen);

} // namespace examvan::handlers::admin
