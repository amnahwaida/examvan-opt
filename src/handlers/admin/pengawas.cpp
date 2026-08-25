#include "handlers/admin/pengawas.hpp"
namespace examvan::handlers::admin {
Response pengawas_exams(const Request&){ Response r; r.json(200,"{\"exams\":[]}"); return r; }
Response pengawas_submissions(const Request&){ Response r; r.json(200,"{\"submissions\":[]}"); return r; }
Response pending_approvals(const Request&){ Response r; r.json(200,"{\"approvals\":[]}"); return r; }
Response set_approval(const Request&){ Response r; r.json(200,"{\"ok\":true}"); return r; }
Response get_auto_approve(const Request&){ Response r; r.json(200,"{\"auto_approve\":false}"); return r; }
Response set_auto_approve(const Request&){ Response r; r.json(200,"{\"ok\":true}"); return r; }
} // namespace examvan::handlers::admin
