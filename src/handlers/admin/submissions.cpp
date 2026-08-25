#include "handlers/admin/submissions.hpp"
namespace examvan::handlers::admin {
Response list_submissions(const Request&){ Response r; r.json(200,"{\"submissions\":[],\"total\":0}"); return r; }
Response submission_detail(const Request&){ Response r; r.json(200,"{\"submission\":null}"); return r; }
Response queue_status(const Request&){ Response r; r.json(200,"{\"pending\":0,\"failed\":0}"); return r; }
Response delete_submission(const Request&){ Response r; r.json(200,"{\"ok\":true}"); return r; }
} // namespace examvan::handlers::admin
