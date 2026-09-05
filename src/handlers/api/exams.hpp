#pragma once
#include "http/router.hpp"
#include "config/config.hpp"
#include "queue/submission_queue.hpp"
#include <functional>
namespace examvan::handlers::api {
Response list_exams(const Request& req);
Response request_approval(const Request& req);
Response exam_by_token(const Request& req);
Response exam_pdf(const Request& req);
Response submit_exam(const Request& req);
Response exam_result(const Request& req);
Response access_log(const Request& req);

// Rate-limit bucket key per exam+MAC (paritas Go ratelimit:presence:/
// ratelimit:submit:); MAC kosong/"unknown" → fallback per exam+IP.
std::string presence_rate_key(const std::string& prefix, int exam_id,
                              const std::string& mac, const std::string& ip);
Response complete_exam(const Request& req);
Response health(const Request& req);
Response time_handler(const Request& req);

/* Test-only hook: tangkap SubmissionJob yang akan di-enqueue oleh submit_exam
 * (tanpa Redis nyata). Dipanggil dari test TDD; tidak dipakai di produksi. */
void set_submit_enqueue_hook_for_test(std::function<void(const queue::SubmissionJob&)> hook);

/* Test-only hook: ganti device_approved() (tanpa PG). nullptr = pakai PG.
 * Dipakai C2 (gate PDF) & submit/result di test TDD. */
void set_device_approved_hook_for_test(std::function<bool(int, const std::string&)> hook);

/* Test-only hook (C3): ganti lookup hasil worker per job_id (tanpa Redis).
 * Return "done:<score>" / "failed:<msg>" / "" (pending). nullptr = pakai Redis. */
void set_result_lookup_hook_for_test(std::function<std::string(const std::string&)> hook);
} // namespace examvan::handlers::api
