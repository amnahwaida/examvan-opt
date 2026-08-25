# F1 — Kontrak Dibekukan (Gerbang F1)

> Status: **BEKU 2026-08-25**. Sumber kebenaran = output Go `examvan-go:bench` 2.7.3-983a6cca (capture `scripts/fixtures/golden/`). Setiap perubahan butuh PR + 2 approver. Metodologi TEST-FIRST: golden → test MERAH → implementasi C++ HIJAU.

## 1. Endpoint HTTP (40+ route, parity class dok 03 §2)

| # | Method | Path | Auth | Parity | Golden |
|---|---|---|---|---|---|
| 1 | GET | `/` | publik | html-structural | `_`.json (29753b, nav + hero) |
| 2 | GET | `/index.html` | publik | html-structural | = `/` |
| 3 | GET | `/robots.txt` | publik | byte-exact | `User-agent: *` |
| 4 | GET | `/login` | publik | html-structural | form + Turnstile |
| 5 | POST | `/login` | RateLimit 10/m + CSRF | json-schema | `{success, message}` 200/401 |
| 6 | POST/GET | `/logout` | CSRF / redirect | 302 `Location:/login` | — |
| 7 | GET/POST | `/register`, `/register/confirm`, `/register/resend` | RateLimit 5/m + CSRF | json/html | OTP flow |
| 8 | GET/POST | `/forgot-password`, `/reset-password` | RateLimit 5/m + CSRF | json/html | — |
| 9 | GET | `/download` | publik | html-structural | `_download.json` 59888b |
| 10 | GET | `/download/apk` | publik | 302 R2 presign | `Location: https://.../examvan-bucket/...` |
| 11 | GET | `/download/app/:id` | RateLimit 60/m | 302 R2 | — |
| 12 | GET | `/hasil` | publik | html-structural | `_hasil.json` 5046b `Cek Hasil` |
| 13 | GET | `/hasil/:token` | publik | html-structural 200/404 | `public_results` guard |
| 14 | GET | `/:token` | publik | 302 → `/hasil/:token` | — |
| 15 | GET | `/api/health` | publik | json-schema | `_api_health.json` `{"status":"healthy","version":"2.7.3-..."}` |
| 16 | GET | `/api/time` | publik | json-schema | `_api_time.json` `{"server_time":"2026-...Z"}` |
| 17 | GET | `/api/exams` | `X-App-Version` + RateLimit | json-schema 426 if < `android_version` | `{"data":[],"pagination":...}` |
| 18 | POST | `/api/exams/request-approval` | version+RateLimit | json-schema | `{"success",...}` |
| 19 | GET | `/api/exams/token/:token` | version | json-schema | `{"token",...}` |
| 20 | GET | `/api/exams/:exam_id/pdf` | version | 302 R2 presign | `X-Amz-*` |
| 21 | POST | `/api/exams/:exam_id/submit` | Limit 5M + version | 202 queued | `{"success", job_id}` |
| 22 | GET | `/api/exams/:exam_id/result` | version | json-schema | `{"score",...}` |
| 23 | POST | `/api/exams/:exam_id/access-log` | Limit 256K | 200 | — |
| 24 | POST | `/api/exams/:exam_id/complete` | Limit 256K | 200 | — |
| 25 | GET | `/api/hasil/:token` | RateLimit 60/m | json-schema | — |
| 26 | POST | `/api/webhook` | HMAC | 200/400 | — |
| 27 | GET | `/ws/:room_id` | RateLimit 60/m + `examvan_session` | 101 Switching | — |
| 28 | GET | `/admin`, `/admin/dashboard`, `/admin/settings` | `AuthRequired` | html-structural + `FeatureLock` | — |
| 29 | GET | `/admin/api/stats` | Auth+FeatureLock | json | — |
| 30 | GET/POST | `/admin/api/users`, `/admin/api/users/:id`, `/admin/api/instansi/update` | `AdminManagementRequired` + CSRF | json | — |
| 31 | GET/POST | `/admin/api/vouchers*`, `/admin/api/packages`, `/admin/api/saas-settings` | `SuperAdmin` + CSRF | json | — |
| 32 | POST | `/admin/api/upload`, `/admin/api/exams*`, `/admin/api/exams/:id/*` | CSRF RateLimit 10/m Limit 102M | json | — |
| 33 | GET | `/admin/api/submissions*`, `/admin/api/queue/status` | FeatureLock | json / XLSX `PK` | — |
| 34 | GET/POST | `/admin/api/pengawas/exams*` | FeatureLock | json | — |

Semua redirect `302 Location` **exact**, JSON `json-schema` (key & tipe sama, urutan bebas), SSR `html-structural` (selector & teks kunci sama).

## 2. Protokol WebSocket (kritikal, dok 03 §3)

Sumber: `internal/websocket/hub.go` 594b.

- **Handshake/auth**: `Cookie: examvan_session=...` HMAC-SHA256 dual-key verifikasi saat `Upgrade`; tolak 401 tanpa sesi valid. `Origin` harus `Host` exact atau `localhost/127.0.0.1`.
- **Room**: `join per exam_id` (string room), `leave` bersih, `GetRoomSize`.
- **Tipe pesan** Socket.IO `["event",payload]`:
  - `ping` → `pong` `2026-...Z`
  - `heartbeat` → `student_update` (sanitize + `Redis heartbeat:{id}:{mac} 5m` + `LPush heartbeats:pending` + fan-out room)
  - `exam_completed` → `DEL heartbeat:{id}:{mac}` + `student_update exam_completed`
  - `join/leave` pengawas, `submission baru`, `countdown` (extend di F3)
- **Backpressure**: `send 256` non-blocking, penuh → `unregister` + `close` (slow-client drop).
- **Reconnect**: `CloseGoingAway/NormalClosure` sama, JS client tanpa ubah.

## 3. Kontrak Sesi Cookie (lintas proses, dok 03 §4)

- Nama `examvan_session`, atribut `Path=/; HttpOnly; Secure; SameSite=Lax` (Go `gin-contrib/sessions/cookie`).
- Serialisasi `securecookie` gob+base64 → `payload_b64.HMAC_SHA256_b64` (OpenSSL). C++ `b64url` fallback + `HMAC` dual-key (`EXAMVAN_SECRET` + `EXAMVAN_SECRET_PREV`) `verify_session_cookie_dual`.
- Rotasi: verifikasi `current || previous`, buat selalu `current`.

## 4. Golden Fixtures (karakterisasi)

`scripts/fixtures/golden/` (capture `docker exec examvan-go-f0 wget -qO-`):

- `_api_health.json` 158b `{"status":"healthy",...}`
- `_api_time.json` 70b `{"server_time":"..."}`  
- `_hasil.json` 5046b `Cek Hasil` html
- `_download.json` 59888b `Download EXAMVAN` html
- `_.json` 29753b `index` html

Test `tests/test_characterization.cpp` bandingkan C++ `Router::dispatch` vs golden (json-schema/html-structural).

## 5. Yang TIDAK Berubah (dok 03 §7)

Skema PG, template `templates/` + `static/` + 68 guard (`1056` di Go), nginx/R2 bucket, alur `review_uiux_webui.md`.

