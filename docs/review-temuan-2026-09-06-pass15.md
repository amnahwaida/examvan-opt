# Review Menyeluruh Seluruh Alur — Temuan Pass-15 (2026-09-06)

Status: **review selesai, belum ada perbaikan kode yang sengaja diterapkan pada pass ini**.

## Baseline

- Test binary dari root project: **681/682 lulus**, 1 skip pre-existing: `P7_Frontend.JsGuardCount`.
- Git working tree bersih pada saat baseline dimulai. Setelah fan-out, tiga perubahan kode uncommitted muncul (`src/db/pool_real.hpp`, `src/handlers/auth/auth_store.cpp`, `src/server/server.cpp`). Perubahan tersebut tidak saya buat sebagai remediation pass-15 dan belum saya audit/tes; jangan commit atau mengandalkannya tanpa review terpisah.
- Cakupan: alur public/auth, student API, submission queue, admin/ownership/CSRF, R2/voucher/settings, websocket/HTTP server, frontend-to-route parity.
- Pembanding: Go reference `/home/vannyezha/project/sekolah/EXAMVAN/webui`, commit `13c0508`.

## Ringkasan prioritas

Pass-14 berhasil menutup sebagian besar celah kritis yang sebelumnya ditemukan: gate PDF, result polling dasar, `job_id` submit, CSRF admin wrapper, R2 dual-layout, dan beberapa scope per-ID. Namun review pass-15 menemukan bahwa beberapa patch hanya berada di wrapper dan belum mencakup route list/bulk/SSR; selain itu ada regresi otorisasi superadmin dan beberapa alur admin yang tidak pernah selesai diparitasikan.

Urutan remediasi yang disarankan:

1. Tutup kebocoran dan mutasi lintas tenant pada admin list/bulk/export/dashboard.
2. Perbaiki model otorisasi `UserCanAccessExam` vs `UserCanControlExam`; pulihkan bypass superadmin yang benar.
3. Perbaiki assignment pengawas/delegation validation dan role handling.
4. Perbaiki voucher redeem yang selalu rollback, lalu bungkus activation dalam transaksi.
5. Tutup student API leakage: protobuf token, approval flood, result identity matching, answer-key first-member.
6. Perbaiki CRLF redirect, login enumeration/lockout, stale HTML sessions, dan CSRF cookie boundary.
7. Pulihkan route/template frontend yang hilang atau disable UI yang tidak didukung.

---

## Temuan HIGH / CRITICAL

### H1. Admin list, bulk, export, dan dashboard masih lintas tenant

**Lokasi:**
- `src/http/router_full.cpp:349,354-355,369,371`
- `src/handlers/admin/exams.cpp:186-224,1142-1192`
- `src/handlers/admin/submissions.cpp:83-149`
- `src/handlers/admin/export.cpp:150-184`
- `src/handlers/admin/dashboard.cpp:82-111,128-155`

Patch C7/C8 hanya memeriksa ownership bila `:id`/`:exam_id` tersedia di path. Route tanpa ID tetap tidak memiliki scope:

- `GET /admin/api/exams` memanggil `list_all()` dan mengembalikan semua exam, termasuk `token`, `active_token`, dan `file_path`.
- `POST /admin/api/exams/bulk-toggle` mengubah ID arbitrary dari array request.
- `POST /admin/api/exams/bulk-delete` menghapus ID arbitrary dari array request.
- `GET /admin/api/submissions` mengembalikan PII semua sekolah; `exam_id` hanya filter opsional.
- `GET /admin/api/submissions/export` dengan `exam_id=0` mengekspor seluruh submission.
- Dashboard SSR juga mengambil `active_store()->list_all()` dan merender token ujian untuk user admin mana pun yang lolos cookie check.
- `dashboard_stats` mengagregasi jumlah exam/storage global.

Go memakai `ListExams`/`FilterAccessibleExamIDs`/`UserCanAccessExam` untuk membatasi owner, delegate, operator instansi yang sama, dan pengawas sesuai route.

**Dampak:** disclosure token, kunci akses ujian, PII siswa, serta perubahan/penghapusan data lintas sekolah.

**Perbaikan:** scope query dan setiap ID bulk di handler/store berdasarkan actor; jangan mengandalkan wrapper path-only. Dashboard dan stats harus menggunakan daftar exam yang accessible.

### H2. Model scope exam salah: pengawas/operator sah diblokir, delegation ordinary guru terlalu luas

**Lokasi:** `src/http/router_full.cpp:142-163,356-380`; `src/handlers/admin/exams.cpp:1208-1343`; `src/handlers/admin/pengawas.cpp:215-406`.

Wrapper `scope="exam"` hanya menerima superadmin, `created_by`, atau `delegated_to`. Go membedakan:

- `UserCanAccessExam`: owner/delegate, pengawas via `exam_pengawas`, dan operator satu instansi untuk monitoring/approval.
- `UserCanControlExam`/`checkExamOwnership`: control actions owner/delegate/operator, bukan pengawas biasa.

C++ memakai satu predicate untuk semua route. Akibatnya pengawas yang memang ditugaskan dan operator satu instansi menerima 403 pada submissions/approvals/auto-approve. Sebaliknya, owner guru biasa dapat melewati wrapper ke `delegate-data`/`delegate`, padahal Go mewajibkan operator/superadmin untuk delegation.

**Perbaikan:** pisahkan access predicate dan control predicate; gunakan membership `exam_pengawas` untuk route monitoring/approval; tambahkan role gate operator/superadmin pada delegation.

### H3. Session superadmin tidak membawa flag superadmin

**Lokasi:** `src/handlers/auth/login.cpp:27-28,184-198`; `src/session/cookie.cpp:135-137`; `src/http/router_full.cpp:148,320-382`.

Login menyimpan role sebagai string JSON `role=["superadmin"]`, tetapi `build_login_session_payload` tidak menulis `is_super_admin`. Parser cookie hanya mengisi `SessionData::is_super_admin` dari field `is_super_admin`, sehingga nilainya tetap false. Wrapper scope per-exam kemudian tidak memberi bypass superadmin; superadmin dapat menerima 403 pada exam questions/delete/toggle/export/pengawas/delegation kecuali mereka adalah owner/delegate.

`create_user` juga memeriksa `sess->role=="superadmin"`, yang tidak cocok dengan role JSON tersebut, sehingga superadmin nyata tidak dapat membuat operator melalui jalur itu.

**Perbaikan:** normalisasi role sekali saat login/session parse; set `is_super_admin` dari canonical role dan gunakan predicate exact yang sama di seluruh middleware/handler.

### H4. Assignment pengawas menerima ID arbitrary dan bisa lintas instansi

**Lokasi:** `src/handlers/admin/exams.cpp:1000-1001,1067-1133,1327-1331`.

`pengawas_ids` hanya diparse sebagai array integer. Tidak ada validasi bahwa target adalah user aktif, role pengawas, atau instansi yang sama. `query_pengawas_json(id,false)` juga mengenumerasi seluruh pengawas aktif global, bukan pengawas satu instansi. Transaction assignment mengabaikan hasil `BEGIN`, `DELETE`, semua `INSERT`, dan `COMMIT`, tanpa rollback jika salah satu insert gagal.

**Dampak:** roster pengawas dapat berisi user lintas sekolah/non-pengawas; UI juga membocorkan daftar akun pengawas global; kegagalan parsial dapat meninggalkan roster tidak konsisten.

**Perbaikan:** validasi setiap target dalam transaksi terhadap `status`, role canonical, dan instansi exam owner; periksa semua hasil SQL dan rollback pada kegagalan; filter available users berdasarkan instansi.

### H5. Voucher redeem selalu gagal/rollback pada PostgreSQL

**Lokasi:** `src/handlers/admin/vouchers.cpp:540-611`, terutama `:597-601`.

Patch M13 sudah menambahkan transaction dan `SELECT ... FOR UPDATE`, tetapi `INSERT INTO voucher_redemptions` tidak memakai `RETURNING`; PostgreSQL mengembalikan `PGRES_COMMAND_OK`, sedangkan kode hanya menerima `PGRES_TUPLES_OK`. Karena itu redeem normal ditandai `__fail__`, di-rollback, dan merespons 500.

**Perbaikan:** terima `PGRES_COMMAND_OK` untuk INSERT tanpa RETURNING atau tambahkan `RETURNING id`; tambah integration test dengan PostgreSQL nyata.

### H6. Public protobuf `/api/exams` masih membocorkan token

**Lokasi:** `src/handlers/api/exams.cpp:263-267`; route `src/http/router_full.cpp:268`.

JSON path sudah menghapus token/file path, tetapi protobuf path masih mengisi `ListExamsResponse.tokens` dengan `active_token`/`token` untuk seluruh exam. Endpoint tidak authenticated dan juga belum memiliki tenant/instansi scope.

**Perbaikan:** ubah protobuf response agar hanya memuat metadata publik yang sama dengan JSON; wajibkan `instansi`/`kode`/`code` dan query berdasarkan `admin_users.instansi_code` atau tambahkan model scope equivalent.

### H7. Student approval request tidak memiliki rate limit per exam/device

**Lokasi:** `src/handlers/api/exams.cpp:413-530`; route `src/http/router_full.cpp:269`.

Go menerapkan bucket Redis per exam dan per device sebelum token check. C++ memvalidasi token dan cap approved device, tetapi tidak memiliki rate limit. Token exam yang bocor dapat membanjiri pending queue dengan MAC berbeda; auto-approve dapat mencetak approved device sampai cap, sedangkan mode manual dapat membanjiri monitoring.

**Perbaikan:** tambahkan bucket `reqapp-exam` dan `reqapp-device` dengan batas/window parity Go; lakukan sebelum token validation.

### H8. Unauthenticated public result redirect rentan CRLF header injection

**Lokasi:** `src/handlers/public/hasil.cpp:393-403`.

`cek_hasil_page` mengambil query `token`, melakukan uppercase, lalu menaruhnya langsung ke `Location: /hasil/<token>`. `parse_form` sudah URL-decode `%0d%0a`; tidak ada validasi token atau URL encoding sebelum header ditulis. Jalur server menulis header value mentah.

**Dampak:** response splitting/header injection melalui `GET /hasil?token=...`.

**Perbaikan:** validasi token dengan allowlist `is_valid_exam_token` sebelum redirect; jangan pernah menaruh nilai query mentah ke header.

---

## Temuan MEDIUM

### M1. Result DB fallback tidak mengikat `identity_data`

**Lokasi:** `src/handlers/api/exams.cpp:911-925`.

Fallback query hanya memakai `exam_id` + `mac_address` dan mengambil submission terbaru. `identity_data` dari query diabaikan (`(void)identity`). Satu device dapat dipakai beberapa siswa; polling dengan `job_id` yang valid namun setelah Redis result hilang dapat menerima score siswa lain dari device/exam yang sama.

**Perbaikan:** cocokkan `job_id` dan/atau identity yang disanitasi sesuai `GetLatestSubmissionByIdentity` Go; jangan fallback ke latest device row tanpa identitas.

### M2. Result polling kehilangan aggregate per-exam rate limit

**Lokasi:** `src/handlers/api/exams.cpp:833-849`.

C++ hanya menerapkan bucket per exam+MAC; Go juga menerapkan aggregate per-exam bucket untuk menahan polling terdistribusi pada satu exam.

**Perbaikan:** tambahkan bucket aggregate `result-exam:<exam_id>`.

### M3. `strip_sensitive_keys` bocor bila key adalah member pertama

**Lokasi:** `src/utils/sanitize.cpp:47-73`; dipakai `src/handlers/api/exams.cpp:630-640` dan `src/handlers/public/hasil.cpp:516-519`.

Implementasi hanya mencari `,"key":`/`,"answer":`. Payload `[{"key":"A","number":1,...}]` mempertahankan key karena tidak ada comma prefix. Format soal dengan urutan field berbeda atau import legacy dapat membocorkan answer key melalui endpoint publik.

**Perbaikan:** parse JSON terstruktur atau dukung object-start `{"key":`/`{"answer":` tanpa merusak nesting. Tambahkan test key-first dan nested array/object.

### M4. Empty submit dapat mengaburkan placeholder monitoring

**Lokasi:** `src/queue/submission_queue.cpp:306-345`.

Worker upsert yang sudah diperbaiki menulis `answers_json="{}"` untuk answers kosong. Ini bisa dianggap submission nyata, bukan placeholder `NULL`, dan dapat overwrite/menyembunyikan state monitoring kosong. Go mempertahankan perbedaan NULL placeholder vs submission.

**Perbaikan:** gunakan SQL NULL untuk answers kosong dan bedakan explicit empty submission dari placeholder sesuai kontrak produk.

### M5. Access-log login masuk queue heartbeat yang seharusnya hanya heartbeat

**Lokasi:** `src/handlers/api/exams.cpp:394-400,1062-1063`; pembanding Go `exams.go:1572-1579`.

Redis key presence memang boleh untuk login, tetapi queue flusher Go hanya menerima event `heartbeat`; C++ `push_heartbeat_presence` perlu dicek tetap hanya LPUSH untuk heartbeat. Review menemukan jalur C++ saat ini sudah memiliki `if(event=="heartbeat")` di helper, sehingga tidak ada duplicate LPUSH pada implementasi terkini. Catatan ini **ditutup sebagai clean setelah verifikasi**, bukan temuan aktif.

### M6. Session HTML admin tidak direvalidasi terhadap status PG

**Lokasi:** `src/http/router_full.cpp:282-320`.

`check_auth` untuk `/admin/dashboard`, `/admin/settings`, `/admin/pengawas`, dan `/admin/submissions` hanya memverifikasi signature cookie. `admin_api` memiliki revalidasi status PG, tetapi halaman HTML tidak. User suspended/deleted dapat tetap membuka halaman admin sampai cookie 24 jam berakhir. Go `AuthRequired` merevalidasi status/role per request.

**Perbaikan:** gunakan middleware revalidasi yang sama untuk page routes; fail closed ketika database production tidak tersedia.

### M7. Login tidak memiliki account lockout dan membocorkan username lewat timing

**Lokasi:** `src/handlers/auth/login.cpp:154-193`.

Pesan error memang seragam, tetapi user yang tidak ada tidak menjalankan bcrypt dummy compare, sementara user valid dengan password salah menjalankan bcrypt. C++ juga tidak memiliki lockout per username 5 gagal/15 menit seperti Go.

**Perbaikan:** selalu jalankan dummy bcrypt untuk unknown user dan tambahkan failed-attempt lockout per account dengan counter atomik/TTL.

### M8. Login masih memakai parsing CSRF lintas Content-Type

**Lokasi:** `src/handlers/auth/login.cpp:107-116`.

Patch M5 memperketat `request_csrf_token` untuk handler public lain, tetapi login memiliki extraction path sendiri: parse form dan JSON tanpa memeriksa Content-Type. Ini lebih longgar dari kontrak Go dan konsistensi handler lain.

**Perbaikan:** gunakan helper CSRF bersama yang memilih parser sesuai Content-Type; tolak body ambiguous/unsupported.

### M9. CSRF cookie dapat dioverride subdomain

**Lokasi:** `src/handlers/auth/login.cpp:71-77`; `src/handlers/admin/template_helper.cpp:~100`.

Cookie `csrf_token` bukan `__Host-` dan tidak memiliki Domain restriction eksplisit. Karena double-submit mengharuskan JavaScript membaca cookie, cookie tidak dapat HttpOnly; subdomain yang dipercaya/tidak aman dapat mencoba menimpa cookie dan memasangkan token baru.

**Perbaikan:** gunakan `__Host-csrf_token` pada production (Secure, Path=/, tanpa Domain), atau pindahkan CSRF ke session server-side.

### M10. Edit PDF filename tidak disanitasi

**Lokasi:** `src/handlers/admin/exams.cpp:715-724,740-754,803`; pembanding create path `:170-184`.

`edit_pdf_name` dari multipart filename digunakan langsung sebagai R2 key dan `file_path`; create path memiliki sanitasi filename tetapi edit path tidak. Filename dengan separator/`..` dapat membuat key/path layout tidak sesuai dan orphan object lama.

**Perbaikan:** basename + allowlist filename + force `.pdf` sebelum upload/store; hapus object lama secara transactional/compensating cleanup.

### M11. Update settings dapat menimpa secret dengan nilai masked

**Lokasi:** `src/handlers/admin/settings.cpp:162-196,228-263`; `static/js/admin.js:3893-3948,3633-3642`.

GET mengembalikan `smtp_password`/`turnstile_secret_key` dalam bentuk masked. Frontend mengisi nilai masked itu kembali lalu mengirim saat save; update backend melakukan upsert mentah. Menyimpan setting lain dapat mengganti secret asli dengan `***...last4`.

**Perbaikan:** server mengenali nilai mask sebagai unchanged atau frontend menghilangkan field masked dari payload.

### M12. `activate_voucher` tidak transaksional dan dapat mengaktifkan redemption kedaluwarsa

**Lokasi:** `src/handlers/admin/vouchers.cpp:633-658`.

Deactivate-all, activate-selected, dan update user dilakukan tanpa `BEGIN`/`ROLLBACK`. Kegagalan langkah kedua/ketiga meninggalkan user tanpa package aktif atau entitlement user/row yang tidak sinkron. Selain itu query redemption tidak memeriksa `is_active`, `remaining_seconds > 0`, atau expiry sebelum aktivasi. Redemption yang sudah kedaluwarsa dapat dipilih kembali dan menulis `expires_at = now() + remaining_seconds` dengan durasi nol/negatif.

**Perbaikan:** satu transaction dengan row/user lock, validasi ownership/status/expiry/remaining duration, rollback semua kegagalan.

### M13. Role arrays arbitrary pada create/edit user

**Lokasi:** `src/handlers/admin/users.cpp:288-323,383-448`.

Singular `role` memiliki allowlist, tetapi array `roles` disimpan tanpa allowlist; payload `roles:["superadmin"]` dapat membuat user privileged melalui route superadmin. `edit_user` juga dapat menulis role arbitrary dan tidak melindungi target superadmin.

**Perbaikan:** canonical role parser allowlist (`guru`, `pengawas`, `operator`, `superadmin` hanya sesuai policy), lindungi akun superadmin dan audit perubahan role.

### M14. Admin API/page authorization fail-open pada beberapa kondisi

**Lokasi:** `src/http/router_full.cpp:63-70,75-103,148-161,167-201,282-320`.

- Valid signed cookie dengan `admin_id=0` dapat lolos jalur direct verification bila PG tidak digunakan; fallback auth yang memeriksa ID tidak selalu dijalankan.
- Revalidasi PG hanya menolak bila `pg_up` true; configured-but-unreachable DB menyebabkan fail-open terhadap stale status pada API.
- Page routes tidak melakukan revalidasi sama sekali.

**Perbaikan:** production fail closed jika DB configured tetapi unreachable; wajibkan `admin_id>0`; centralize auth/revalidation.

### M15. `show_answers=0` tetap memberikan correctness detail dan identity data ke public result API

**Lokasi:** `src/handlers/public/hasil.cpp:555-590`.

C++ selalu mengirim `identity_data` dan `evaluated_answers` walau anonymous dan `show_answers=false`. Ini sama dengan Go reference saat ini, tetapi secara product/security policy berarti anonymous viewer dapat melihat identitas custom dan status benar/salah per soal. Jika policy `show_answers` dimaksudkan menutup seluruh correctness detail, response harus menghapus/gate kedua field tersebut.

### M16. HTML pengawas detail belum dirender dan endpoint audit hilang

**Lokasi:** `templates/admin/pengawas_detail.html:83,149,157,1094,2018-2020`; `src/handlers/admin/template_helper.cpp:7-26`; `src/http/router_full.cpp:375-380`.

Tidak ada `templates/admin/pengawas_detail.rendered.html`. Loader fallback hanya mengganti `{{.version}}`, sehingga placeholder Go (`{{.exam.ID}}`, `{{range...}}`, conditional expressions) dikirim mentah ke browser. Template juga memanggil `GET /admin/api/pengawas/exams/:id/audit-logs`, tetapi tidak ada route/handler C++.

**Dampak:** halaman monitoring pengawas rusak atau memiliki ID literal; tab audit selalu 404. Ini correctness/availability, bukan data leak langsung.

**Perbaikan:** port template ke renderer C++/buat rendered template valid dengan data server-side; implementasikan audit endpoint atau hapus UI call.

### M17. `/admin/submissions` memakai CSRF token rendered stale

**Lokasi:** `src/handlers/admin/submissions.cpp:67-79`; `templates/admin/submissions.rendered.html:251-252`; `src/http/router_full.cpp:315-320`.

Halaman submissions membaca rendered HTML langsung dan tidak memanggil `render_admin_page`, sehingga tidak membuat cookie/token CSRF segar. Mutasi dari halaman dapat gagal 403 atau bergantung pada token hardcoded/stale.

**Perbaikan:** gunakan `render_admin_page` dan set `Set-Cookie` seperti dashboard/settings/pengawas.

### M18. Logout form normal selalu dapat 403

**Lokasi:** `templates/admin/partials/nav.html:82-85`, `templates/admin/dashboard.rendered.html:363-368`, `templates/admin/submissions.rendered.html:251-256`, `src/handlers/auth/logout.cpp:18-37`.

Handler logout menerima hidden field `_csrf`/`_csrf_token`, tetapi rendered nav memuat token hardcoded yang tidak sama dengan cookie segar. Tidak ada JS yang mengintersep `.dropdown-logout` untuk menambahkan header. Akibatnya submit form normal tidak cocok dengan cookie dan logout gagal.

**Perbaikan:** render hidden token yang sama dengan cookie segar pada seluruh rendered pages, atau gunakan JS `apiFetch`/header CSRF untuk logout.

---

## Temuan LOW / INFO

- `GET /admin/api/saas-settings/test-smtp` dipanggil `static/js/admin.js:3750` tetapi route C++ tidak ada (`router_full.cpp:323-324`).
- `settings-system-apps.js` mengharapkan list/upload/delete system apps, tetapi C++ hanya memetakan `/admin/api/system-apps` ke `settings_page`/`update_settings` (`router_full.cpp:381-382`), bukan handler system-apps.
- Frontend mengharapkan `/admin/api/pengawas/state`, tetapi route tidak terdaftar.
- `instansi` masih belum menjadi kolom/model exam C++; scope penuh lintas sekolah belum dapat dicapai hanya dengan owner/delegate.
- `delete_submission` mengembalikan sukses walau `DELETE` memengaruhi zero rows atau PG tidak tersedia (`submissions.cpp:218-237`); wrapper ownership mencegah sebagian IDOR, tetapi kontrak response menyesatkan.
- `bulk_delete_exams` hanya menghapus layout `exams/{id}/{file}`; single delete memiliki dual-layout cleanup. Legacy `pdfs/{file_path}` dapat menjadi orphan.
- `change_password`, voucher redeem/activate/mine, dan beberapa management routes C++ lebih ketat daripada Go (superadmin-only), menyebabkan availability/correctness gap bagi user biasa.
- C++ `instansi_update` hanya mengubah current user dan meng-echo `name` tanpa `json_escape` (`users.cpp:631-648`), berbeda dari operasi instansi Go.
- Token rotation di `exam_by_token` menggunakan in-process store lock, bukan DB row lock; multi-instance/dual-run dapat last-write-wins pada `active_token`.
- `start_time` submit belum menerapkan sanitasi/normalisasi yang sama dengan Go.

## Pass-14 fixes yang diverifikasi clean

- C2 PDF: token + active/started + schedule + approval gate + rate limiting ada (`api/exams.cpp:664-730`).
- C3 result: access gate, Redis result, DB fallback, pending/done/failed response ada; namun M1 identity matching masih kurang.
- C4 submit: `job_id`, `congrats_message`, dan `start_time` sudah dipasang (`api/exams.cpp:780-850`).
- C6 R2: PDF read memilih `pdfs/{file_path}` lalu fallback layout C++ dan delete paths sudah diperluas.
- M2 heartbeat SAVEPOINT poison-loop fix dan M3 worker upsert sudah ada; empty-answer distinction masih perlu diperbaiki.
- H1 uWS body state saat ini memakai `UwsRequestState` per request, `responded/aborted` guard, dan close-after-413 (`server.cpp:402-454`).
- M7 stub `/ws/:room_id` sudah dihapus; route HTTP fallback tidak lagi memantulkan room JSON.
- M8 queue menyimpan binary flag dan uWS mengirim protobuf sebagai BINARY (`server.cpp:487-492`).
- M11 router tidak lagi melakukan URL decode parameter dua kali.
- M10 presign R2 fail-closed endpoint check dan expiry clamp ada.
- M4 OTP compare constant-time, M5 shared auth handlers, M6 atomic OTP attempts sudah ada; login masih memiliki parser CSRF sendiri.

## Rekomendasi pengujian berikutnya

1. Tambahkan integration tests dengan PostgreSQL untuk voucher redeem/activate, ownership, assignment roster, dan multi-tenant query.
2. Tambahkan route parity smoke test dari seluruh `static/js/*.js`/template fetch URL terhadap `router_full.cpp`.
3. Tambahkan public API tests untuk JSON dan protobuf yang memastikan tidak ada token/file path/key-first leakage.
4. Jalankan staging flow dengan dua instance C++ dan rollback Go untuk active-token/R2 compatibility.
5. Tambahkan browser test untuk login → dashboard → submissions → logout, termasuk CSRF cookie regeneration.
6. Setelah fix admin scope, ulangi test matrix: owner, delegate, assigned pengawas, operator same instansi, operator different instansi, superadmin, suspended user, and missing DB.

---

*Review pass-15 selesai 2026-09-06. Tidak ada code fix yang sengaja diterapkan pada pass ini; tiga perubahan uncommitted yang muncul selama sesi dicatat di bagian Baseline dan belum diaudit.*
