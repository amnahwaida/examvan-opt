# Review Menyeluruh Seluruh Alur — Temuan Pass-18 (2026-09-06)

Baseline: HEAD `101978e` (lanjutan Pass-17), ref Go `EXAMVAN/webui` 2.7.4.
Metode: 4 subagent paralel (public/auth, admin, WS/queue/infra, config/frontend/tests) + verifikasi manual `main.cpp`, `server.cpp`, `router_full`, `docker-compose`, `nginx.conf`.
Scope: HTTP+WS+queue+jobs+DB/Redis+R2+nginx/docker+frontend/tests.

## Executive summary

Pass-17 menutup 26/28 temuan Pass-16, tapi review Pass-18 menemukan sisa **fail-open sistemik** (rate-limit tanpa Redis, Turnstile tanpa token, admin_api fail-open vs halaman fail-closed), **auth/session gap** (tanpa expiry server-side, logout tak clear Secure, CSRF dual-skema), **infra durability** (submit LPUSH-only tanpa INSERT, requeue void, tanpa graceful shutdown, Hub redis_ctx shared, SETNX tak dipakai), dan **parity uWS vs posix** (static traversal guard hanya di posix, header allowlist buang auth/IP, frame limit beda). Tidak ada yang menutup temuan P17-C1..C7/M1..M7 — semuanya masih relevan dan ditambah di bawah.

## Prioritas remediasi

1. Graceful shutdown + drain queue/batch/flusher/jobrunner (`main.cpp:150`, `server.cpp:505-559`, `submission_queue.cpp:315-343`, `jobs.cpp:12-21`).
2. Submit durability: INSERT-before-LPUSH + cek LPUSH prod + idempotency submit (`api/exams.cpp:129-143`, `submission_queue.cpp:269-297`, `main.cpp:126-129`).
3. Fail-open → fail-closed: rate-limit tanpa Redis, Turnstile kosong, admin_api vs check_auth (`api/exams.cpp:308-329`, `auth/login.cpp:145-152`, `router_full.cpp:83-111,339,344`).
4. Session: tambah `exp`, perbaiki logout Secure/__Host-, satukan CSRF (`session/cookie.cpp:126-144`, `auth/logout.cpp:50-59`, `auth/login.cpp:77,131` vs `auth_helpers.cpp:82-83`).
5. uWS static traversal guard + header forwarding penuh + samakan limit (`server.cpp:357-375,397-401,460`).
6. Redis thread-safety Hub + SETNX jobs + pool reuse (`main.cpp:79-83`, `redis/client.cpp:13-28`, `jobs.cpp:25,57,82`, `submission_queue.cpp:559-564`).

---

## CRITICAL

- **C1. Session tanpa expiry — replay indefinite.** `session/cookie.cpp:126-144`, `auth/login.cpp:239-240`: payload hanya `admin_id/username/role`, tanpa `exp/iat`. `Max-Age` hanya klien. Revokasi hanya cek `status=active` di `admin_api` (fail-open bila PG down).
- **C2. Logout tak hapus cookie Secure prod.** `auth/logout.cpp:50,54` clear tanpa `Secure`/`__Host-`, sedang `login.cpp:241` set `; Secure`. `GET /logout:57-59` hanya 302 tanpa clear.
- **C3. Submit LPUSH-only, jendela hilang 1s.** `api/exams.cpp:129-143,901-907`: tanpa INSERT dulu. `docker-compose.yml:26` `appendfsync everysec`. Koneksi baru per-request tanpa pool.
- **C4. Requeue void di prod — job hilang diam-diam.** `main.cpp:126-129`, `submission_queue.cpp:269-297`: `set_lpush_checked` tak pernah di-set prod; gagal LPUSH return `true`.
- **C5. Tanpa graceful shutdown.** `main.cpp:150` `while(true)sleep(24h)` tanpa signal; `Server::stop/Worker::stop/Flusher::stop/JobRunner::stop` tak terjangkau. `server.cpp:505-507,551-559` detach + `joinable()` selalu false; posix worker detached `:523-538`.
- **C6. uWS static tanpa traversal guard.** `server.cpp:357-375` vs posix `:200-239` (5x double-decode + tolak `..,\0,%2e,%252e`). uWS `"."+path` → `/static/../server/server.cpp`, `/%2e%2e/` lolos.

## HIGH

- **H1. Rate-limit student fail-open tanpa Redis.** `api/exams.cpp:308-329,493-495,534,788,834,1157,1235,973`: `!ctx → return true`. Tanpa Redis unlimited + flood queue.
- **H2. Turnstile fail-open bila token kosong.** `auth/login.cpp:145-152` hanya verify `if(!turnstile.empty())`. `middleware/turnstile.cpp:52` `secret.empty→false` tak tercapai.
- **H3. CSRF dual-skema.** `login.cpp:77,131` `__Host-` prod, tapi `auth_helpers.cpp:82-83` hanya `csrf_token` + `template_renderer.cpp:185` selalu plain tanpa `Secure` → register/recovery rentan subdomain planting.
- **H4. `client_ip()` spoofable.** `auth_helpers.cpp:20-28` percaya `X-Real-IP/XFF` tanpa trusted-proxy; klaim `router_full.cpp:243-247` tak ditegakkan.
- **H5. Hub redis_ctx shared antar thread.** `main.cpp:79-83` satu ctx untuk semua WS (`hub.cpp:192,197,223`); hiredis tidak thread-safe. Queue sudah benar `thread_local:122-129`.
- **H6. Job lock in-process bukan SETNX.** `redis/client.cpp:13-28` hanya `unordered_map`; `redis_real::redis_setnx:20-22` ada tapi `jobs.cpp:25,57,82` tak pakai → multi-replika dobel-run expiry/cleanup/retention.
- **H7. WS student heartbeat selalu drop?** `hub.cpp:152,208` `if(!privileged)return`, `priv=admin_id!=0` (`server.cpp:105,466`). Verifikasi vs Go: relay pengawas vs direct-student.
- **H8. uWS drain tanpa backpressure check.** `server.cpp:487-492` tanpa `getBufferedAmount()/onWritable`; `maxBackpressure=1M:462` tanpa handler → close diam-diam.
- **H9. `delegate_exam` tanpa validasi.** `admin/exams.cpp:1357-1362` INSERT `exam_pengawas` mentah (vs `save_exam_questions:1136-1139` cek active/role/instansi).
- **H10. Audit-log hanya SELECT.** `admin/pengawas.cpp:434`; grep INSERT nihil di `src/` → audit selalu kosong, aksi admin hanya `log_info` stderr.
- **H11. Scoring `json_bool_field` gagal bila ada spasi.** `middleware/scoring.cpp:95-102` `compare(c+1,4,"true")` → `"partial_scoring": true` jadi false; partial mati diam-diam.
- **H12. `Settings.smtp_password` di wire proto.** `proto/examvan.proto:119`; bila `GetSettings` protobuf tak di-redact → bocor kredensial.
- **H13. Voucher batch selalu 500.** `admin/vouchers.cpp:306-312` cek `TUPLES_OK` untuk INSERT tanpa RETURNING (`COMMAND_OK`) → `created_ok=0`.
- **H14. Posix `flush_queue` abaikan `send()` return.** `server.cpp:122-133`: block/SIGPIPE/partial-write tak ditangani.
- Referensi tetap OPEN dari P17: C1 voucher redeem buntu (`router_full.cpp:396-398`), C2 change-password/instansi superadmin-only, C3 system-app 5MB vs 100MB (`router_full.cpp:51` vs `settings.cpp:344`), C4 approval tak di-revoke (`submission_queue.cpp:340-375` vs Go `submission_queue.go:462-466`), C5 tanpa UNIQUE `(exam_id,mac)`, C6 nested `active_store()` deadlock, C7 identity_fields required.

## MEDIUM

- **M1. Fail-open/fail-closed PG inkonsisten.** `router_full.cpp:83-111` fail-open dev vs `:339,344` fail-closed halaman. `is_authenticated:middleware/auth.cpp:4-13` tanpa cek status.
- **M2. Register/recovery hanya `parse_form`.** `register.cpp:115`, `recovery.cpp:81,162` vs `login.cpp:113-115` JSON. `wants_json:auth_helpers.cpp:89-94` kontrak JSON tapi diabaikan.
- **M3. Rate-limit ganda tak sinkron** (`router_full.cpp:248-264` + `register.cpp:302,358` + `recovery.cpp:98,170`); urutan CSRF vs RL terbalik forgot (`recovery.cpp:82,98`).
- **M4. Header allowlist uWS buang sinyal.** `server.cpp:397-401,443-447` teruskan X-Exam-Token/XFF/X-Real-IP tapi buang `Authorization/User-Agent/Referer/X-Forwarded-Proto`; posix `:300-323` parse semua → 401/426 palsu + IP audit mati. `complete_exam` tanpa `pb_gate` (`router_full.cpp:311-317`).
- **M5. Frame limit beda.** posix 5MB (`server.cpp:149,170,176`), uWS 64KB (`:460`). Dokumentasikan/seragamkan.
- **M6. `verify_session_cookie` fallback seluruh header** (`cookie.cpp:127-128`); seharusnya nullopt bila `examvan_session` absen.
- **M7. Submission/detail tanpa cek in-handler**, 100% scope router (`router_full.cpp:195-230`, `submissions.cpp:169-255`, `pengawas.cpp:230-289`); split PG/memory → 403 salah / bypass.
- **M8. `edit_user` mass-assignment + `roles_json` superadmin** (`users.cpp:415-430,109,295-298,325,347`); gate superadmin membatasi tapi hardening kurang. `bulk-toggle/delete:router_full.cpp:428-429,exams.cpp:1180,1206` abaikan `exam_pengawas`/operator-instansi.
- **M9. Fallback tanpa PG return success no-op** (`pengawas.cpp:375,420`, `submissions.cpp:254`, `users.cpp:452,487,630,650`).
- **M10. Assignment best-effort 200 palsu** (`exams.cpp:1125,1151`); `activate __invalid__ → 503` seharusnya 400 (`vouchers.cpp:650,668-672`).
- **M11. Backoff serial block batch** (`submission_queue.cpp:427` sleep di `run_batch`); `batch_q_.size():359` tanpa lock (race `:345`).
- **M12. Flusher/pool per-tick + leak.** `submission_queue.cpp:559-564` `connect_redis+RealPool(60)` per tick; `pool_real.cpp:31-36` tanpa dtor `PQfinish`. `db/pool_real.cpp:16-28` ping stale, acquire unbounded, tanpa mutex; worker per-batch `RealPool(10):376-377` + jobs per-run 60 (`jobs.cpp:59-74,84-93`) → storm. Flusher tanpa coalesce MAC sama.
- **M13. `migrate()` hanya exams/idempotency** (`exam_store_postgres.cpp:149-188`); submissions/logs/approvals/admin_users diasumsikan milik Go → fresh-DB gagal. `hydrate:190-199` hanya LIMIT 1 (nama menyesatkan).
- **M14. `connect_redis` parse naif** (`redis_real.cpp:5-14`: stoi throw, abaikan password/db/TLS, tanpa reconnect).
- **M15. nginx rollback = outage.** `nginx.conf:7` `go_backend down`; `sed s/cpp_backend/go_backend/` arahkan semua ke down. `map $request_uri` sertakan query-string. `location /:41-55` Upgrade untuk semua + `read_timeout 60s` < WS idle 120s → WS diputus; hilang `X-Forwarded-Proto/Host`; `/api/health:71-80` tanpa Host/XFF; HSTS tanpa TLS.
- **M16. Docker/CI.** `compose:50` DB_PASSWORD di env (docker inspect); Redis tanpa requirepass; db/redis/nginx tanpa user/read_only; nginx root; `Dockerfile:21` `libhiredis-dev` di runtime + hack libstdc++ `:25-26`; filter test kecualikan P3-P13/R3-R5/Docker/Review (`Dockerfile:14`); CI tanpa uWS/protobuf/curl (`ci.yml:35,39`), label 248 usang, preseden `-o` salah, tanpa `nginx -t`/scan.
- **M17. Config.** `env_int:config.cpp:7-12` telan stoi gagal → default diam-diam; handler baca `getenv(DATABASE_URL)` langsung bypass validate (`hasil.cpp:82,449,531`, `users.cpp:132,330`, `settings.cpp:310`, `auth_store.cpp:156`); R2 strict getenv mentah (`exams.cpp:425,767`); protobuf default 0 vs dok MANDATORY (`.env.example:20`, `compose:61`, `config.cpp:42-43`); `WITH_PROTOBUF=ON` tapi QUIET tanpa FATAL (`CMakeLists:89-95`).
- **M18. Frontend/proto.** `protobuf-helper.js:11-15` CDN unpkg tanpa SRI; `apiFetchProtobuf:34-46` tak auto-encode/retry 415; error `PROTOBUF_REQUIRED` selalu JSON (`protobuf.cpp:32,44`); XHR system-apps tak dispatch `api:error` (`settings-system-apps.js:414-415`, `admin-core.js:253-254`).
- **M19. Contract drift.** `extract_contract.py:6` path absolut mati; `contract.json:1-146` 40 route `auth:unknown`; vs `router_full.hpp` (`/admin/api/stats,packages,system-apps,questions,token-mode,vouchers/batch` tak ada). `parity_harness.py:11` hanya 4 path; shadow tak assert protobuf. CI lewati ~200 TEST.
- P17-M1..M7 tetap OPEN/PARTIAL (fallback `__Host-`→plain, token rotation mutex lokal, mandatory protobuf hanya admin, logout `_csrf_token` mismatch, operator lintas instansi, TOFU identity, audit key `data` vs `logs`).

## LOW / INFO

- L1 `search` ILIKE tanpa escape `%/_` → wildcard enum (`hasil.cpp:550,558`); `cek_hasil_api:591-593` + `json_escape:40-55` tak escape `<>&` (aman hanya bila `textContent`).
- L2 Status HTML vs JSON: confirm gagal 302 hilang konteks (`register.cpp:315,322,334`) vs reset 400 render (`recovery.cpp:201-202`); email `@+.` lemah tanpa length cap (`register.cpp:37-42`); `GET /:token` oracle sintaks (`router_full.cpp:292-298`, bukan open-redirect — `next` di `login.cpp:258-266` sudah baik).
- L3 `mask_token` sized-mask bocor panjang (`settings.cpp:86-90`); klaim P17-L5 fixed tak sesuai. Skip-write `*` + fallback SMTP baik (`:259-261,282-284`).
- L4 `export_xlsx` tanpa LIMIT OOM (`export.cpp:179`); `export_csv` dummy (`:145-149`); voucher custom_* JSON→0 + `created_by "NULL"` (`vouchers.cpp:240-247,262-263`); `list_packages` protobuf dead code (`:708`); rendered snapshot berisi csrf nyata — jangan commit.
- L5 JobRunner/flusher sleep tanpa wakeup (`submission_queue.cpp:583-592`, `jobs.cpp:12-21`); retry tanpa cek LPUSH (P17-L2/L3), shutdown BRPOP window (P17-L4).
- L6 `.env` nyata di root (gitignored, berisi R2/DB plaintext — rotasi bila pernah `add -f`); `.env.example` hilang TURNSTILE/CORS/STORAGE/ALLOW_MEMORY/BYPASS; download fallback `test.r2` info-leak (`download.cpp:27,47,51`); version hardcode `2.7.2` vs `2.7.3` (`template_renderer.cpp:165-167`, `template_helper.cpp:12-13`); uWS content-type hilang ico/woff/proto + tanpa Content-Length; `body_limit.cpp:4` text/plain tanpa error_code; scoring `extract_raw_value/parse_questions` naif, `total 0 → 0` vs nullopt; `sanitize_student_input` hanya collapse WS (`helpers/utils.cpp:31-40`); `strip_sensitive_keys` hanya `key/answer`; `copyCode` textarea hygiene; `EXAM_TOKEN` butuh jsEscape (`hasil.html:300`).

## Yang sudah baik (pertahankan)

- HMAC dual-key + CSRF constant-time (`cookie.cpp:71-73`, `csrf.cpp:19-25`), OTP via `verify_csrf`, dummy-hash + lockout 5/15m (`login.cpp:169-179,187-188`), `is_valid_exam_token` + decode-sekali (`router.cpp:32-48`), semua SQL `exec_params`, sanitize MAC/field paritas Go, `strip_sensitive_keys` JSON-aware, list tak bocor token, PDF gate token+approved+jadwal+grace, Turnstile fail-closed bila secret ada.
- Voucher redeem/activate `FOR UPDATE+BEGIN/COMMIT`, anti-oracle, tolak superadmin/operator-created; R2 fail-closed + endpoint clamp + verify HEAD + retry + orphan cleanup; PDF magic `%PDF/%%EOF` + `sanitize_filename` + `.pdf` paksa; settings partial `json_has_key` + skip `*`; CSV formula-prefix, XML strip control; `save_questions` validasi tipe + transaksi WIB→UTC.
- WS posix: origin+Key wajib, 5MB frame/fragmen, pong, double-decode guard, body 5MB+413; uWS per-request state anti-bocor keep-alive, dual-secret, 64K/1M limit, BINARY vs TEXT; Hub queue 256 drop-close + snapshot broadcast.
- Queue: TX+savepoint+advisory+ON CONFLICT, poison-guard heartbeat, backoff+`lpush_checked` hook, expiry tombstone `inactive`; infra: PG/Redis healthcheck, AOF everysec, nginx limit_req/conn + 5m + headers, `validate()` SECRET≥32, fail-closed tanpa PG, compose `?ERR` + read_only + no-new-priv + tmpfs + USER examvan.
- Scoring satu sumber server-side; CORS tolak `*`; `admin-core` auto CSRF + 401→expired; `escapeHtml` + `__proto__` guard; `WORKING_DIRECTORY=root` untuk test path-relatif.

## Rekomendasi pengujian

1. Shutdown/drain: `docker stop` saat COMMIT/BRPOP/batch pending; final drain + wakeup CV.
2. Durability: Redis down saat submit, DB down setelah dequeue, FK violation, cross-job result + identity fingerprint.
3. Auth/session: expiry/replay, logout Secure/__Host-, CSRF Host vs plain mismatch, suspended/pending_otp, PG down (API vs halaman).
4. Rate/turnstile: tanpa Redis, XFF spoof, token kosong, JSON vs form register/recovery.
5. uWS vs posix: traversal `/static/../`, header X-Exam-Token/XFF, body >5MB, frame >64KB, backpressure 1MB.
6. Admin matrix: owner/delegate/assigned/operator same/diff/superadmin/suspended + bulk + export LIMIT + voucher batch + system-app 100MB + audit tab + SMTP.
7. Dual-instance token rotation `FOR UPDATE`, protobuf mandatory student routes, R2 PDF compat, browser login→dashboard→submissions→logout form.
