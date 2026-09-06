# Review Menyeluruh Seluruh Alur — Temuan Pass-17 (2026-09-06)

Status: **remediation lanjutan; tree saat ini di `101978e` bersih (hanya `.claude/` untracked), sebagian besar temuan CRITICAL pass-16 terverifikasi FIXED, beberapa regresi fungsional baru diperkenalkan oleh hardening putaran terakhir**.

## Scope dan baseline

- Repository target: `/home/vannyezha/project/sekolah/examvan-opt`, HEAD `101978e fix(security): accept __Host- CSRF cookie, harden mask/sanitize paths`.
- Reference Go: `EXAMVAN/webui`, commit `13c05081bde133a6b681bed4865375b43ffd547f` (release 2.7.4).
- Remediation commits sejak `83e5729` (baseline pass-15): `989aadf`, `076d35b`, `c97c72b`, `7597000`, `123b8a4`, `3a77609`, `dfa9d9b`, `51b55c2`, `96fcf38`, `5efee4e`, `6f10d5a`, `101978e`.
- Test baseline: `tests/` berisi 57 file, 683 deklarasi `TEST`/`TEST_F` (konsisten dengan klaim 681/682 passed + 1 skip `P7_Frontend.JsGuardCount`); full suite tidak dijalankan dalam review ini, hanya inspeksi.
- Metode: fan-out 4 subagent (public/auth, admin, queue/WS/infra, frontend/tests) + verifikasi manual terhadap current tree + diff commit `101978e`. Klaim yang hanya dari snapshot lama tidak dimasukkan.

## Executive summary

Pass-16 memperbaiki integritas queue async yang paling kritikal (publish-before-persist, false-202, binding job_id, poison heartbeat, R2 presign, uWS 413). Pass-17 memverifikasi: **26/28 temuan pass-16 terverifikasi FIXED**, 2 tetap OPEN (`P16-C7` identity validation, `P16-M3` protobuf mandatory student routes), 1 PARTIAL (`P16-M5` token rotation multi-instance).

Remediasi pass-16/17 sendiri memperkenalkan **regresi fungsional baru**: voucher redemption terblokir total, change-password/instansi self-service terblokir, upload system-app >5 MB selalu 413, logout via form browser → 403, serta kelemahan fallback `__Host-` → plain cookie.

Sisa risiko integritas: tidak ada UNIQUE constraint pada `submissions(exam_id, mac_address)` → duplikasi row pada race; approval tidak di-revoke setelah commit → device dapat re-enter tanpa izin baru; hot-retry tanpa backoff saat DB down.

## Prioritas remediasi (berikutnya)

1. Buka kembali voucher redeem/activate/mine dan change-password/instansi untuk role non-superadmin (regresi fungsional HIGH).
2. Naikkan batas body wrapper untuk `/admin/api/system-apps` atau bypass wrapper untuk route tersebut (HIGH fungsional).
3. Tutup duplikasi submission (UNIQUE index + ON CONFLICT) dan revoke approval setelah commit worker.
4. Validasi `identity_fields` required sebelum enqueue (P16-C7).
5. Terapkan `require_protobuf` pada student routes bila `protobuf_mandatory=1` (P16-M3).
6. Perbaiki token rotation ke `SELECT ... FOR UPDATE` (P16-M5/NEW-04) dan deadlock nested `active_store()` (Q-NEW-1).

---

## Status matriks temuan Pass-16

| ID | Judul | Severity | Status saat ini | Bukti |
|---|---|---|---|---|
| P16-C1 | Worker publish success sebelum persist DB | CRITICAL | **FIXED** (dengan edge durability, lihat Q-NEW-5/6) | `submission_queue.cpp:318-372` commit PG sebelum `store_result(success=true)`; savepoint per-job, retry `kMaxRetries=3` |
| P16-C2 | Submit 202 palsu saat enqueue Redis gagal | CRITICAL | **FIXED** | `api/exams.cpp:128-142` `enqueue_job_to_redis():bool`, `796-802` → 503 bila gagal |
| P16-C3 | Result job_id tidak terikat exam/device | CRITICAL | **FIXED** (core) / **PARTIAL** identity | `api/exams.cpp:889-896` validasi `job_id/exam_id/mac`, `931-935` fallback `WHERE job_id=$1 AND exam_id=$2 AND mac_address=$3`; `identity_data` masih `(void)` — TOFU |
| P16-C4 | Protobuf list bocor token | CRITICAL | **FIXED** | `api/exams.cpp:253-271` field `tokens` kosong pada branch protobuf |
| P16-C5 | Approval flood tanpa rate limit | HIGH | **FIXED** | `api/exams.cpp:463-467` bucket `reqapp-exam` 12000 + `reqapp-device` 30 |
| P16-C6 | Protobuf submit tanpa job_id | HIGH | **FIXED** | `proto/examvan.proto:356-357`, `api/exams.cpp:812-820` |
| P16-C7 | Submit tanpa validasi identity_fields required | HIGH | **OPEN** | `api/exams.cpp:771-795` tidak iterasi `exam.identity_fields`; Go `exams.go:1044-1087` menolak required kosong |
| P16-M1 | Rate limit bypass via MAC arbitrary | MEDIUM | **FIXED** | `api/exams.cpp:747-754` `sanitize_mac_like_go()` sebelum `rate_limit_allowed` |
| P16-M2 | Answer-key stripping gagal first-member | MEDIUM | **FIXED** | `sanitize.cpp:49-74` cek `{` atau `,` sebelum strip |
| P16-M3 | Mandatory protobuf tidak ke student routes | MEDIUM | **OPEN** | `router_full.cpp:265-299` `require_protobuf` hanya admin; student POST tanpa gate |
| P16-M4 | Worker shutdown tinggalkan batch | MEDIUM | **FIXED** (edge window kecil) | `submission_queue.cpp:293-328` `while(running_||pending()>0)` drain; last in-flight BRPOP masih race — Q-NEW-6 |
| P16-M5 | Token rotation in-process locking | MEDIUM | **PARTIAL** | `api/exams.cpp:588-604` `st.update()` mutex lokal; multi-instance last-write-wins |
| P16-M6 | Edit PDF filename tanpa sanitasi | MEDIUM | **FIXED** | `admin/exams.cpp:726-735` `sanitize_filename()` |
| P16-A1 | Admin list/bulk/export/dashboard lintas tenant | HIGH | **FIXED** (dengan catatan operator global — lihat A-NEW-6) | Wrapper `exam`/`exam_access` + filter `created_by/delegated_to`/operator same-instansi |
| P16-A2 | Predicate pengawas/operator vs control | HIGH | **FIXED** | `router_full.cpp:148-177,356-380` scope `exam` vs `exam_access` terpisah |
| P16-A3 | Session superadmin tanpa flag | HIGH | **FIXED** | `auth/login.cpp:32-35`, `session/cookie.cpp:125-148` derive `is_super_admin` + fallback |
| P16-A4 | Assignment pengawas arbitrary ID | HIGH | **FIXED** (save_exam_questions) / **PARTIAL** delegate_exam masih tanpa validasi transaksi penuh | `admin/exams.cpp:1066-1133` validasi active/pengawas/instansi |
| P16-A5 | Voucher redeem selalu rollback | HIGH | **FIXED** | `admin/vouchers.cpp:595-602` cek `PGRES_COMMAND_OK` + RETURNING |
| P16-A6 | Voucher activation non-transaksional | MEDIUM | **FIXED** | `admin/vouchers.cpp:635-665` transaksi + `FOR UPDATE` + cek expiry |
| P16-A7 | Arbitrary roles pada create/edit user | MEDIUM | **FIXED** | `admin/users.cpp:288-323,383-448` allowlist |
| P16-A8 | Masked secrets timpa secret asli | MEDIUM | **FIXED** | `admin/settings.cpp:165-265` skip bila berisi `*` |
| P16-A9 | Admin auth fail-open tanpa positive ID | MEDIUM | **FIXED** | `router_full.cpp:74-107` `admin_id<=0` → 401; page revalidate PG |
| P16-A10 | Pengawas detail/template/CSRF/audit | MEDIUM/LOW | **FIXED** (inti) | `template_helper.cpp:10-40`, `admin/pengawas.cpp:408-480`, `router_full.cpp:378-385`; sisa mismatch field key — A-NEW-7 |
| P16-A11 | Frontend route parity gaps | LOW | **FIXED** (inti) | `router_full.cpp:381-395`, `admin/settings.cpp:270-355` |
| P16-P1 | /hasil redirect CRLF | HIGH | **FIXED** | `public/hasil.cpp:395-405` allowlist `is_valid_exam_token()` |
| P16-P2 | Login timing & lockout | MEDIUM | **FIXED** | `auth/login.cpp:25-28,168-188` lockout 15m/5x + dummy bcrypt |
| P16-P3 | Login CSRF parser tanpa Content-Type | MEDIUM | **FIXED** | `auth/login.cpp:113-127` branch JSON vs form |
| P16-P4 | HTML admin tanpa revalidasi session | MEDIUM | **FIXED** | `router_full.cpp:285-325` revalidate PG |
| P16-P5 | CSRF cookie tanpa `__Host-` | MEDIUM | **FIXED** | `auth/login.cpp:77-84`, `template_helper.cpp:84-90`; fallback weakness — A-NEW-5 |
| P16-P6 | Public result bocor identity saat show_answers=0 | MEDIUM | **FIXED** | `public/hasil.cpp:555-585` sembunyikan identity/evaluated |

---

## Temuan baru Pass-17 — HIGH / CRITICAL

### P17-C1. Voucher redemption terblokir total — tidak ada role yang bisa redeem — **HIGH (fungsional)**
**Lokasi:** `router_full.cpp:396-398`, `admin/vouchers.cpp:537,613`.
`POST /admin/api/vouchers/redeem|activate` dan `GET /admin/api/vouchers/mine` di-gate `admin_api(...,"superadmin")` → hanya superadmin lolos. Handler `redeem_voucher` menolak superadmin (`role.find("superadmin") → "__super__" → 403 "Akun SuperAdmin tidak dapat menukar kode voucher"`). Hasil: route hanya untuk superadmin, handler menolak superadmin → **tidak ada user yang bisa redeem/activate/mine**. Regresi dari hardening role-gate.
**Perbaikan:** gate redeem/activate/mine ke role guru/pengawas/operator (bukan superadmin), atau ubah handler agar superadmin diizinkan sesuai kebijakan produk; tulis integration test redeem → activate end-to-end.

### P17-C2. Self-service change-password & instansi_update terblokir untuk non-superadmin — **MEDIUM (fungsional)**
**Lokasi:** `router_full.cpp:391-392`, `admin/users.cpp:594,633`.
Kedua handler mengubah akun sesi sendiri (`uid` dari session), tetapi route di-gate `superadmin`. Guru/pengawas/operator tidak bisa ganti password atau update instansi sendiri.
**Perbaikan:** gate kedua route ke `""` (authenticated) atau `guru|pengawas|operator` sesuai Go parity; verifikasi `admin.js` caller.

### P17-C3. Upload system-app >5 MB selalu 413 — **HIGH (fungsional)**
**Lokasi:** `router_full.cpp:51`, `admin/settings.cpp:344`.
Wrapper `admin_api` menolak `req.body.size()>5*1024*1024` → 413 sebelum handler. System-app mengizinkan 100 MB (`file.size()>100*1024*1024`). Upload APK 100 MB tidak pernah sampai handler.
**Perbaikan:** naikkan batas untuk `/admin/api/system-apps` (atau bypass wrapper untuk route tersebut), atau streaming multipart tanpa buffer penuh.

### P17-C4. Worker tidak revoke approval setelah commit — device dapat re-enter tanpa izin baru — **HIGH**
**Lokasi:** `queue/submission_queue.cpp:340-375` (Go `submission_queue.go:462-466` melakukan `DELETE FROM exam_approvals WHERE exam_id=$1 AND mac_address=$2` setelah commit).
C++ publish `done` tanpa menghapus `exam_approvals`. Device tetap `approved` selamanya → bisa download PDF lagi dan re-submit tanpa persetujuan pengawas.
**Perbaikan:** setelah `COMMIT` sukses, `DELETE` approval untuk setiap job yang persist sukses.

### P17-C5. Duplicate submission rows tanpa UNIQUE constraint — **HIGH**
**Lokasi:** `store/exam_store_postgres.cpp:165-166`, `queue/submission_queue.cpp:351-357,464-478`.
Tidak ada `UNIQUE(exam_id, mac_address)` pada `submissions`; worker upsert `UPDATE ... ORDER BY created_at DESC LIMIT 1` lalu `INSERT`; heartbeat placeholder (`drain_heartbeat_batch:464-478`) dan worker upsert berlomba dalam tick yang sama → 2 row untuk pasangan yang sama. 8 worker + advisory lock per-txn tidak mencegah race antar transaksi.
**Perbaikan:** tambah `UNIQUE INDEX submissions_exam_mac` + `INSERT ... ON CONFLICT (exam_id, mac_address) DO UPDATE`.

### P17-C6. Deadlock potensial submit/approval via nested `active_store()` — **HIGH**
**Lokasi:** `api/exams.cpp:342-375,768`, `store/exam_store_postgres.cpp:244`, `store/exam_store_memory.cpp:89-92`.
`submit_exam` memegang `*exam` lalu memanggil `device_approved() → device_has_approval() → store::active_store()` lagi; `ExamStorePostgres::update` mengunci `mu_` selama flash-in + PG round-trip. Dua mutasi bersamaan dapat deadlock pada mutex yang sama.
**Perbaikan:** hindari re-entry: snapshot exam sebelum lock, atau pisahkan read path tanpa `active_store()` di dalam update.

### P17-C7. Submit tanpa validasi identity_fields required (duplikat P16-C7, tetap OPEN) — **HIGH**
**Lokasi:** `api/exams.cpp:771-795`.
Lihat P16-C7. Dampak: submission tanpa NISN/identitas wajib tetap ter-queue dan merusak roster/export.

---

## Temuan baru Pass-17 — MEDIUM

### P17-M1. Fallback `__Host-csrf_token` → `csrf_token` melemahkan proteksi subdomain — **MEDIUM**
**Lokasi:** `auth/login.cpp:127-131`, `http/router_full.cpp:126-128`, `auth/logout.cpp:31-32` (commit `101978e`).
Jika korban belum punya cookie `__Host-` (sebelum load pertama atau setelah clear), attacker di subdomain dapat set `csrf_token` via `Domain=.site` + kirim header `X-CSRF-Token` yang cocok (double-submit tanpa binding session) → CSRF lolos. Akar: cookie CSRF tidak diikat ke session.
**Perbaikan:** hilangkan fallback plain cookie di production, atau ikat token CSRF ke session server-side.

### P17-M2. Token rotation race multi-instance — **MEDIUM** (duplikat P16-M5)
**Lokasi:** `api/exams.cpp:588-604`.
`st.update()` hanya mutex in-process; 2 node generate token 8-char berbeda → last-write-wins, siswa di node lain token invalid.
**Perbaikan:** `SELECT ... FOR UPDATE` di PG, update DB dulu lalu sync memori.

### P17-M3. Mandatory protobuf tidak ke student routes — **MEDIUM** (duplikat P16-M3)
**Lokasi:** `router_full.cpp:265-278`, `middleware/protobuf.cpp:45-75`.
`require_protobuf()` hanya admin; student `request-approval/submit/access-log` tetap terima JSON saat `PROTOBUF_MANDATORY=1`.
**Perbaikan:** bungkus student POST dengan `require_protobuf(cfg, ...)` bila flag aktif.

### P17-M4. Logout via form browser → 403 (field name mismatch) — **MEDIUM**
**Lokasi:** `templates/admin/partials/nav.html:83` (`name="_csrf_token"`), `auth/logout.cpp:23` (mencari `csrf_token`/`_csrf`/`csrf`).
Form `POST /logout` native (tanpa JS intersepsi; `admin-core.js:apiFetch` hanya untuk XHR) mengirim `_csrf_token` → handler tidak menemukannya → 403. CSRF header `X-CSRF-Token` tidak ada pada submit form biasa.
**Perbaikan:** tambah `"_csrf_token"` ke daftar field di `logout.cpp` atau ubah template ke `csrf_token`.

### P17-M5. `pengawas_exams` operator melihat semua exam lintas instansi — **MEDIUM**
**Lokasi:** `admin/pengawas.cpp` branch `is_priv` (`role` contains `operator`/`superadmin` → tanpa filter).
Operator instansi A melihat exam instansi B beserta token. Go membatasi same-instansi.
**Perbaikan:** filter operator non-superadmin ke `WHERE instansi = $caller_instansi` atau `EXISTS` same-instansi.

### P17-M6. Result binding masih TOFU untuk identity — **MEDIUM**
**Lokasi:** `api/exams.cpp:856-858,889-896,947`, `queue/submission_queue.cpp:352`.
`identity_data` diparse lalu `(void)`; fallback `WHERE job_id=$1 AND exam_id=$2 AND mac_address=$3` tanpa identity. Pembindingan Redis hanya replay dari job, bukan FK ke row submission.
**Perbaikan:** simpan fingerprint identity di `JobResult` dan validasi di `/result`; fallback harus `AND identity_hash=$4` atau `submission_id`.

### P17-M7. Audit-log response key mismatch — timeline kosong — **LOW**
**Lokasi:** `admin/pengawas.cpp:427` (`{"data":[...]}`), `templates/admin/pengawas_detail.html:1862` (`res.logs || []`).
Frontend audit modal membaca `res.logs`, backend mengirim `data` → `logs` undefined → "Belum ada riwayat" selamanya.
**Perbaikan:** samakan ke `logs` (atau `data`) di kedua sisi.

---

## Temuan baru Pass-17 — LOW / INFO

### P17-L1. Upload system-app 201 vs frontend 200 — toast "Gagal" palsu — **LOW**
**Lokasi:** `admin/settings.cpp:354` (`201`), `static/js/settings-system-apps.js:432` (`xhr.status===200` only).
Upload sukses 201 dianggap gagal di UI.
**Perbaikan:** terima `200||201` atau ubah backend ke 200.

### P17-L2. Retry submission tanpa backoff — hot loop saat DB down — **LOW**
**Lokasi:** `queue/submission_queue.cpp:371,374`, `submission_queue.hpp:17` (`kMaxRetries=3` tanpa delay).
Gangguan DB → retry hammer tiap `brpop(5)`.
**Perbaikan:** exponential backoff atau delay requeue.

### P17-L3. Requeue tidak cek hasil LPUSH — job hilang bila Redis down — **LOW**
**Lokasi:** `queue/submission_queue.cpp:268-279`, `main.cpp:120`.
`requeue()` return `true` tanpa cek LPUSH; job yang gagal karena Redis down hilang setelah retry habis.
**Perbaikan:** periksa reply LPUSH, return false dan persist fallback bila gagal.

### P17-L4. Shutdown window last in-flight BRPOP — job hilang — **LOW**
**Lokasi:** `queue/submission_queue.cpp:293-328`.
`stop()` join worker sebelum batch thread; BRPOP yang sedang blokir bangun setelah `run_batch` exit → job hilang.
**Perbaikan:** urutan stop: `running_=false`, join batch dulu lalu worker, atau final drain setelah join.

### P17-L5. `mask_token` baru (101978e) bocorkan panjang secret pendek — **INFO**
**Lokasi:** `admin/settings.cpp:85-88` (`<=4` → `****` sepanjang secret).
Perbaikan mencegah verbatim leak (bagus), tapi panjang tetap bocor. Masked value mengandung `*` → `update_settings` skip (benar untuk "unchanged"), tetapi secret legit yang mengandung `*` tidak bisa diupdate tanpa workaround.

### P17-L6. Test `LoginMismatchFails` diganti `LoginAcceptsHostPrefixedCookie` — coverage mismatch hilang — **INFO**
**Lokasi:** `tests/test_csrf_fix_tdd.cpp:148-164`, `tests/test_characterization.cpp:145-155`.
Test mismatch dihapus, diganti accept-host. Rejection masih tertutup oleh `F5Login.CsrfMismatch403` (legacy cookie) tapi Host-prefixed mismatch tidak diuji.
**Perbaikan:** kembalikan test mismatch untuk `__Host-csrf_token` + negative test fallback plain-cookie dengan header yang tidak cocok.

---

## Hal yang diverifikasi tetap baik

- PDF gate: token + active/started + schedule + `device_approved` + R2 configured + rate limit (`api/exams.cpp:664-720`).
- Submit membangun job dengan answers/identity/start_time + enqueue 503 (`api/exams.cpp:722-811`).
- Heartbeat poison: per-row `SAVEPOINT hb_row` rollback (`submission_queue.cpp:426-495`), bukan requeue 500-batch.
- Router tidak double url-decode; `presign_url` region `auto` + fail-closed endpoint (`r2.cpp:27-48`).
- uWS body per-request + `responded/aborted` guard + `close-after-413` (`server.cpp:402-454`); WS protobuf pong binary flag benar (`hub.cpp:125-131`, `server.cpp:487-492`).
- Login lockout + dummy bcrypt, JS guard `show_answers=false` menyembunyikan identity/evaluated.

## Cakupan yang masih perlu verifikasi PostgreSQL/Redis (belum integration test)

- Tenant queries penuh + assigned-pengawas access + bulk authz + render audit tab di staging PG.
- System-app multipart upload + R2 cleanup metadata; browser logout/form token.
- Durable queue commit/retry, result binding identity, voucher transaksi, migrasi skema.

## Rekomendasi pengujian

1. Integration PG+Redis: Redis down saat submit, DB down setelah dequeue, FK violation, shutdown dengan batch pending, cross-job result.
2. Result binding: job/exam/device/identity fingerprint termasuk JSON vs form submit.
3. JSON/protobuf parity untuk list exams dan mandatory protobuf mode.
4. Key-first answer stripping + MAC canonicalization per test yang sudah ada.
5. Admin matrix: owner/delegate/assigned pengawas/operator same/different instansi/superadmin/suspended/DB unavailable.
6. Browser route: login → dashboard → submissions → logout (form), pengawas detail audit tab, system-apps upload 201, test SMTP.
7. Dual-instance token rotation + R2 PDF compat.

---

## Remediation progress — 2026-09-06 (pass-17 delta)

Implemented sejak pass-16 dan terverifikasi di `101978e`:
- CSRF production memakai `__Host-csrf_token` dengan fallback `csrf_token` (`auth/login.cpp:127-131`, `router_full.cpp:126-128`, `auth/logout.cpp:31-32`); `AdminMutationAcceptsHostCsrfCookie` + `LoginAcceptsHostPrefixedCookie` hijau.
- `mask_token` tidak pernah verbatim untuk secret `<=4` (`admin/settings.cpp:85-88`).
- `submit mac_address` melalui `sanitize_mac_like_go` (`api/exams.cpp:778,791`).
- Regex versi system-app tanpa double-escape (`admin/settings.cpp:340`).

Masih terbuka / memerlukan batch implementasi berikutnya:
- Seluruh P17-C1..C7 dan P17-M1..M7 di atas; tidak ada finding yang dianggap production-closed tanpa integration PG/Redis yang lolos.
