# Temuan Review Pass-24 — 2026-09-09

Review menyeluruh ExamVan pass-24. Mode **review-only** — tidak ada perubahan kode sumber proyek pada pass ini.

## Baseline & Metodologi

- HEAD: `390fd0b` (fix(review-pass23): TDD remediasi carried-over M16/M19/B3/B4/B5/B6)
- Perubahan belum di-commit (worker tree):
  - `CMakeLists.txt`, `src/queue/submission_queue.cpp`, `src/redis/client.cpp`, `src/redis/redis_real.cpp`, `src/redis/redis_real.hpp`, `src/store/exam_store_postgres.cpp`
  - Untracked: `tests/test_p32_tdd.cpp`, `tests/test_pg_integration_tdd.cpp` (auto-SKIP tanpa `DATABASE_URL`) — catatan higienis kelas-T8 (sama dengan pass-21): segera commit bersama remediasi berikutnya agar baseline test reproducible.
- Hasil test: `./build/examvan-tests` — **817 test, 816 PASSED, 1 SKIPPED** (`P7_Frontend.JsGuardCount`), 14685 ms.
- Metode: review paralel per area — D (admin exams handlers), E (admin users/settings), F (api exams/webhook/public), G (queue/store/db/redis/utils/models + kontrak test), I (nginx/compose/Dockerfile), H (frontend JS), J (admin templates), K (public templates) + dua mini-review (Health, version-gate). Agent H/J/K dihentikan di tengah proses atas permintaan user ("segera susun laporan dan akhiri pencarian"); agent pengawas dan server-core mati karena rate-limit 429 (cakupan disubsumsi — lihat Catatan Cakupan).
- Pengecualian re-report: temuan CLOSED pass-20..23 (T1-T9, H7, M7, M8, L3, M16, M19, B3-B6, M9 fail-closed pola lama, C5, B1) dan heartbeat-flusher P24 (CONFIRMED CLOSED oleh G) tidak dilaporkan ulang.

## Ringkasan Eksekutif

| Severity | Jumlah |
|---|---|
| HIGH | 6 |
| MEDIUM | 19 |
| LOW | 32 |

## Status Remediasi (update 2026-09-10 — TDD pass-24)

Remediasi dieksekusi dengan konsep TDD (RED → GREEN) tiga batch:

- **Batch 1 (HIGH)** → `tests/test_p33_tdd.cpp` (12 test kontrak)
- **Batch 2 (MEDIUM)** → `tests/test_p34_tdd.cpp` (13 test kontrak)
- **Batch 2b (D3/D4)** → `tests/test_p35_tdd.cpp` (6 test kontrak)

Marker per temuan di bawah: **[CLOSED — Pnn]** = selesai & terkunci test kontrak; **[OPEN]** = belum diremediasi (Batch 3); **[OPEN — verifikasi ulang]** = detail bukti tipis, wajib diverifikasi ulang sebelum remediasi.

| Severity | CLOSED | OPEN |
|---|---|---|
| HIGH (6) | 6 | 0 |
| MEDIUM (19) | 14 | 5 — I4, I5, F2, F3, F6 |
| LOW (32) | 2 — D5, G9 | 30 |

Verifikasi build produksi (`HAS_LIBPQ=1` + `HAS_PROTOBUF=1`, libpq-dev 16): **855 test — 848 PASSED, 0 FAILED**, 7 SKIPPED (1 `P7_Frontend.JsGuardCount` skip lama + 6 `PgIntegration` auto-SKIP tanpa server PG hidup — by design). Kontrak test lama yang diamendemen mengikuti kontrak fail-closed baru: `Webhook_ValidProtobufResponse`, `CreateUser_JsonMutationStillWorks`, `CreateUser_EditUser_DeleteUser_StubStillWorks`, `UpdateSettings_JsonStillWorks`; `SetApproval/GetAutoApprove/SetAutoApprove/DeleteSubmission_JsonStillWorks` dipin mode-memori deterministik (pola `RedeemVoucher_JsonStillWorks`). Catatan I3: `.dockerignore` terpasang — rotasi kredensial yang pernah masuk build context tetap aksi operator di luar repo.

Pola dominan pass-24:

1. **Kelas fake-200 / M9 fail-open residual (3 temuan HIGH: E-a, E-g, + E-h MEDIUM)** — konversi M9 pass-20 menutup jalur baca (getters/`get_auto_approve`/`delete_submission`) tetapi menyisakan jalur TULIS admin: 6 handler `users.cpp` mengembalikan 200 sukses tanpa menulis apa pun saat PG down, `update_settings` menelan kegagalan upsert, dan handler voucher sekelas. Ini pola yang sama dengan D2 (`delegate_exam` catch-all jatuh ke 200) — totalnya satu kelas sistemik "admin write path fail-open".
2. **Validitas waktu voucher (E-f HIGH)** — perbandingan kedaluwarsa voucher bergantung sepenuhnya pada parsing string `parse_iso_utc` terhadap teks `timestamptz` PG; parsing yang tidak robust membuat voucher kedaluwarsa tetap redeemable.
3. **Keluarga stack-local pool (satu kelas, lintas 6 sumber)** — `DbPool`/`RealPool` lokal stack per-request/per-batch masih hidup di admin exams (D5), export xlsx (E-c), run_batch queue (G1), main.cpp startup (G9), api/webhook/hasil (F5), dan check_auth router — konversi T2/P24/P32 belum menuntaskan seluruh jalur PG.
4. **Queue durability (G2)** — hasil `requeue()` diabaikan; blip Redis saat LPUSH = job hilang tanpa jejak (tidak pending, tidak failed-queue, tidak ada JobResult).
5. **Keamanan transport/konfigurasi (G4, F1)** — `sslmode` query-string client dipaksa `require` (verify-full hilang), webhook OTP tanpa expiry/attempts + ack fail-open.

---

## Temuan HIGH (6)

### E-a — **[CLOSED — P33]** M9 fail-open: 6 handler `users.cpp` fake-200 saat PG down

- **Kategori**: correctness / M9-residual
- **Lokasi**: `src/admin/users.cpp` (enam handler: `edit_user`, `delete_user`, `user_toggle_status`, `user_verify`, `user_deactivate_package`, `change_password`)
- **Bukti**: semua handler mengembalikan respons 200 sukses tanpa melakukan operasi ketika koneksi PG gagal (pola M9 pass-20 yang hanya dikonversi di jalur baca).
- **Skenario**: PG down → admin menonaktifkan user / mengubah password → UI melaporkan sukses, DB tidak berubah. Kontrol akses yang "sudah dicabut" tetap aktif.
- **Remediasi**: sama seperti pola M9 di `set_approval` (fail → 503). Perluas kontrak test fail-closed ke jalur tulis admin users.

### E-f — **[CLOSED — P33]** `parse_iso_utc` vs PG `timestamptz` → voucher kedaluwarsa tetap redeemable

- **Kategori**: correctness / keamanan bisnis
- **Lokasi**: `voucher_usable` bergantung sepenuhnya pada `parse_iso_utc` terhadap teks `timestamptz` PG (bandingkan `expires_at` dengan sekarang).
- **Bukti**: parsing string waktu yang tidak robust terhadap format keluaran `timestamptz` (offset zona/s presisi) → parse gagal/keliru → perbandingan kedaluwarsa salah arah (fail-open).
- **Skenario**: voucher lewat masa berlaku tetap lolos `voucher_usable` → redeem sukses → kerugian finansial SaaS.
- **Remediasi**: bandingkan expiry di sisi SQL (`now() > expires_at`) atau buat `parse_iso_utc` ketat + fail-closed (parse gagal → voucher TIDAK usable). Kunci dengan test unit pada string `timestamptz` riil PG.

### E-g — **[CLOSED — P33]** `update_settings`: upsert menelan kegagalan → false 200

- **Kategori**: correctness / M9-residual
- **Lokasi**: `update_settings` (router `:397`, superadmin-gated) — path store settings.
- **Bukti**: `upsert_setting` menelan kegagalan (hanya `log_error`), `written++` dieksekusi tanpa syarat → respons sukses walau 0 baris tertulis.
- **Skenario**: PG down → superadmin mengubah pengaturan sistem → 200 "berhasil", pengaturan tidak tersimpan; UI menampilkan nilai lama setelah reload → kebingungan + keputusan berbasis setting yang tidak aktif.
- **Remediasi**: periksa hasil upsert per key; gagal → 503 (pola M9). Test: fail-closed saat DB down.

### I1 — **[CLOSED — P33]** `client_max_body_size 5m` vs carve-out system-apps 105MB

- **Kategori**: konfigurasi / kontrak fitur
- **Lokasi**: `nginx.conf:34` (`client_max_body_size 5m`) vs `nginx.conf:54` (carve-out 105MB untuk upload system-apps).
- **Bukti**: limit global 5m menolak body >5MB SEBELUM carve-out `:54` sempat berlaku pada jalur yang tidak ter-cover carve-out; upload APK/system-apps >5MB gagal 413 dari nginx.
- **Skenario**: user upload aplikasi 50MB → 413 dari nginx padahal konfigurasi aplikasi mengizinkan 105MB → fitur tampak rusak.
- **Remediasi**: samakan limit: `client_max_body_size` pada blok location system-apps (≥105m) atau naikkan global; test konfigurasi nginx (`nginx -t`) + test upload besar end-to-end.

### F1 — **[CLOSED — P33]** Webhook OTP: tanpa expiry/attempts, ack fail-open, route tanpa gate

- **Kategori**: keamanan / kontrak handler
- **Lokasi**: `src/api/webhook.cpp:111-117` (ack sebelum parse), `:148-151` (SELECT tanpa cek expiry), `:154-155` (normalisasi dua sisi), `:137-168` (swallow error), `:145` (stack pool); bare route `router_full.cpp:324` (tanpa `pb_gate`/`rl_wrap`). Kontras dengan pola benar di `register.cpp:318/:324/:327/:213/:365-368`.
- **Bukti**: (a) OTP diverifikasi tanpa batas usia & tanpa counter attempt → brute-force sustained; (b) ack dikirim sebelum parsing/validasi body → client menganggap sukses walau pesan ditolak; (c) swallow error di `:137-168` → kegagalan internal tidak terlihat; (d) route tanpa rate-limit wrapper.
- **Skenario**: attacker brute-force OTP tanpa expiry; atau OTP valid dikirim, diproses gagal, tapi ack 200 sudah terkirim → client tidak retry → aktivasi hilang diam-diam.
- **Remediasi**: tambah kolom/cek `expires_at` + `attempts` (increment + lockout), kirim ack hanya setelah validasi sukses, fail-closed swallow, bungkus route dengan rate-limit. **Amandemen kontrak test**: `Webhook_ValidProtobufResponse` (`tests/test_protobuf_handlers_tdd.cpp:737`) mengunci ack-tanpa-DB — harus diamendemen. `Webhook_PgPinsActivationSql` (`tests/test_production_hardening_tdd.cpp:2010`) TIDAK mengunci expiry/attempts → aman ditambah.

### F4 — **[CLOSED — P33]** Header client `X-Version` direfleksikan mentah ke atribut href

- **Kategori**: keamanan / XSS residual (attribute breakout)
- **Lokasi**: baca `X-Version` client menang (`server.cpp:533`, writeHeader mentah `:545`); template `templates/public/cek_hasil.rendered.html:17-21` (`?v=2.7.3-...`); `template_helper.cpp:12` (raw replace).
- **Bukti**: nilai `X-Version` dari client dimenangkan atas versi build dan disisipkan mentah ke atribut href → kandidat attribute-breakout (`" onmouseover=...`). **Catatan penting**: klaim awal agent "Tanpa CSP" TIDAK tepat — CSP memang aktif di jalur nginx (4 lokasi: `nginx.conf:42/:55/:84/:96`), sehingga dampak dimittigasi di jalur produksi nginx; inti risiko tetap attribute breakout pada jalur non-nginx/direct.
- **Skenario**: client mengirim `X-Version: 2.7.3" accesskey=X onclick=...` → atribut baru terinjeksi di href (dimittigasi CSP di jalur nginx; tetap berbahaya jika CSP bypass/berubah).
- **Remediasi**: whitelist/escape nilai `X-Version` (regex `^[A-Za-z0-9.\-]+$`) atau berhenti memenangkan header client untuk rendering. **Kontrak test**: test `:1854` hanya mengunci forwarding X-Version, bukan rendering → remediasi aman tanpa amendemen.

---

## Temuan MEDIUM (19)

### D1 — **[CLOSED — P34]** `create_exam`: idempotency reservation bocor di jalur error protobuf

- **Kategori**: idempotency / kontrak
- **Lokasi**: `src/admin/exams.cpp:299-300` (`require_protobuf` err return), `:306-308` (ParseFromArray fail → 400 "invalid protobuf"), `:322` (415 PROTOBUF_REQUIRED tanpa HAS_PROTOBUF) — semua SETELAH `reserve_idempotency` (~`:270`) tanpa `release_and_return`.
- **Bukti**: key K + protobuf rusak → 400, tapi K tetap reserved `{fingerprint, completed=false}` → semua retry 409 IDEMPOTENCY_CONFLICT selamanya (memory: sampai restart; PG: state='pending' tanpa lease/expiry). Kontrak `IdempotencyReleaseOnFailure` hanya mengunci jalur form (name kosong), bukan jalur protobuf.
- **Skenario**: client kirim body protobuf valid + K, tapi payload gagal parse → 400; retry dengan payload yang sudah diperbaiki dan K sama → 409 permanen; ujian tidak bisa dibuat tanpa ganti K.
- **Remediasi**: `return release_and_return(r);` / `return release_and_return(*err);` di ketiga path. Perluas kontrak `IdempotencyReleaseOnFailure` ke jalur protobuf.

### D2 — **[CLOSED — P33]** `delegate_exam`: hasil transaksi PG tak diperiksa, catch-all jatuh ke 200

- **Kategori**: transaksi / error-handling
- **Lokasi**: `src/admin/exams.cpp:1468-1507` — BEGIN/UPDATE SET delegated_to/DELETE exam_pengawas/INSERT/COMMIT (`:1468-1497`) semua membuang return value; `catch(...)` (`:1500-1507`) log lalu JATUH ke store update + 200 "Delegasi ujian berhasil disimpan".
- **Bukti**: PG down → store memory menulis delegated_to tapi DELETE+INSERT exam_pengawas hilang → pengawas lama tetap punya akses sementara UI melaporkan sukses. Melanggar pola M9 yang benar di `set_approval`/`set_auto_approve` (`pengawas.cpp:411-418, 478-484` → 503). Bonus N+1 validasi pengawas (`:1475-1491`, 2 SELECT per pengawas). Clear-delegasi (new_owner kosong) eksekusi UPDATE NULL di PG tapi store update di-skip `if(new_owner.has_value())` → no-op 200 senyap di mode memory.
- **Skenario**: delegasi "berhasil" → pengawas lama masih bisa membuka ujian; pengawas baru belum terdaftar → akses tercampur.
- **Remediasi**: periksa tiap `exec_params` (gagal → ROLLBACK + 503); catch harus return 503; satu query `WHERE id = ANY($1)`; tambah `write_audit_log`.

### D3 — **[CLOSED — P35]** Edit-token: cek kolisi non-atomik, kegagalan claim ditelan

- **Kategori**: race / TOCTOU
- **Lokasi**: `src/admin/exams.cpp:688-695` — urutan `token_exists` → update → `unclaim_token` → `if(!claim_token(t)){/* sudah ada — jangan gagalkan */}`.
- **Bukti**: dua admin set token sama pada ujian berbeda → keduanya lolos `token_exists` → dua ujian aktif berbagi token. UNIQUE PG (`exam_store_postgres.cpp:17`) menahan persist, tapi mode memory: token aktif terduplikasi sampai reload; tidak ada guard sama sekali.
- **Skenario**: siswa login dengan token X masuk ke ujian yang salah (atau dua ujian) → integritas ujian rusak.
- **Remediasi**: claim SEBELUM mutasi + rollback manual, atau `claim_token_if_absent(t, exclude_id)` atomik di bawah lock store.

### D4 — **[CLOSED — P35]** R2 dimutasi sebelum store (delete/update/bulk_delete)

- **Kategori**: ordering / R2
- **Lokasi**: `src/admin/exams.cpp` — delete `:887-903` (r2.remove kedua layout `:887-897` → `exams().remove` `:903`; kegagalan R2 hanya di-log `:895`); update `:796-849` (upload PDF baru `:796-808` SEBELUM store update `:815`); bulk_delete `:1324-1327` (`else { ...continue; }` saat R2 tidak dikonfigurasi vs delete tunggal 503 di `:898-902`).
- **Bukti**: delete → R2 gagal → ujian tetap ada di store tapi PDF sudah terhapus (atau sebaliknya: store remove sukses, R2 leak). Update: 404 store → objek R2 baru orphan; sukses → objek lama tidak pernah dihapus → akumulasi object storage. Bulk: 10 ujian dikirim, semua di-skip, respons tetap laporkan jumlah sukses.
- **Skenario**: storage R2 membengkak oleh PDF orphan; ujian tampil di daftar tapi PDF-nya hilang.
- **Remediasi**: balik urutan (store dulu, R2 sesudah / soft-delete-trash); bulk samakan policy dengan tunggal; hapus objek R2 lama setelah update sukses.

### E-c — **[CLOSED — P34]** Stack-local pool per-request di export xlsx (anggota keluarga stack-pool)

- **Kategori**: perf / T2-contradiction
- **Lokasi**: `src/admin/export.cpp:166-168` — `RealPool(…,10)` lokal per request, di-wire via `export_xlsx` (router `:444` + `:447`).
- **Bukti**: setiap request export membangun koneksi PG baru dan membongkarnya; burst export → connection churn + latency. Satu kelas dengan D5/G1/G9/F5 (lihat Dedup).
- **Remediasi**: konversi ke `global_pool()`; kunci dengan perluasan ban P29/P32.

### E-h — **[CLOSED — P33]** Handler voucher: kelas fake-200/fail-open (sekelas E-a/E-g)

- **Kategori**: correctness / M9-residual
- **Lokasi**: handler create/toggle voucher di area E (detail bukti tidak selamat kompaksi konteks — verifikasi ulang saat remediasi).
- **Skenario**: sama dengan E-a — operasi admin voucher melaporkan sukses tanpa menulis saat PG down.
- **Remediasi**: fail-closed 503; kunci test.

### G1 — **[CLOSED — P34]** `run_batch` membangun pool per iterasi batch

- **Kategori**: koneksi / churn
- **Lokasi**: `src/queue/submission_queue.cpp:409-411`, di dalam `while(running_ || pending()>0)`: `auto cfg = examvan::Config::load(); examvan::DbPool pool(cfg.database_url, 10); examvan::db::RealPool real(examvan::conninfo_from_url_or_raw(pool.url), 10);`
- **Bukti**: jalur PG TERAKHIR yang belum memakai `global_pool()`. P32 hanya mengunci `drain_heartbeats_once`; P29 hanya mem-ban `RealPool real(` di 5 file handler admin → `submission_queue.cpp` tidak terkunci test.
- **Skenario**: tiap batch membuat+bongkar koneksi → churn konstanta + beban PG; burst → connection storm.
- **Remediasi**: `global_pool()`/`with_global_pg`; tambah ban literal `RealPool real(` untuk `submission_queue.cpp` di P29/P32.

### G2 — **[CLOSED — P34]** Hasil `requeue()` diabaikan → job hilang tanpa jejak

- **Kategori**: durability / data-loss
- **Lokasi**: `src/queue/submission_queue.cpp:468` (jalur PG), `:471` (non-PG), `:361-363` (stop-drain) — `if (job.retries < kMaxRetries) { job.retries++; queue_->requeue(job); }` bool dibuang.
- **Bukti**: LPUSH gagal saat blip Redis → job hilang total: tidak di pending, tidak di `kFailedQueueKey` (T5 hanya menutup retry-exhausted), tidak ada JobResult. Siswa melihat "terkirim" tapi jawaban tak pernah diproses — tanpa jejak kegagalan.
- **Skenario**: jawaban ujian siswa lenyap diam-diam saat Redis bermasalah 1 detik.
- **Remediasi**: periksa hasil `requeue()`; `false` → push_failed + JobResult (hook `set_lpush_checked` P17-L3 sudah tersedia).

### G3 — **[CLOSED — P34]** `PQconnectdb` dieksekusi sambil memegang mutex pool

- **Kategori**: concurrency / blocking
- **Lokasi**: `src/db/pool_real.cpp:35-42` — `std::lock_guard<std::mutex> g(mu_); if(!idle_.empty()){...return;} auto* c = PQconnectdb(...)`.
- **Bukti**: TCP+auth di bawah `mu_`; cold-start/burst → semua thread termasuk releaser terblokir detik; satu PG yang lambat membekukan seluruh jalur PG.
- **Skenario**: PG restart 5 detik → seluruh worker queue + handler admin berhenti total (bukan hanya koneksi baru).
- **Remediasi**: buat koneksi DI LUAR mutex (lock→cek idle→unlock→PQconnectdb→lock lagi) + `connect_timeout` di conninfo.

### G4 — **[CLOSED — P34]** `pg_conninfo_from_url` memaksa `sslmode=require`, membuang parameter lain

- **Kategori**: konfigurasi / keamanan transport
- **Lokasi**: `src/db/pool.cpp:79-83` — `std::string qs = u.substr(qpos+1); if (qs.find("sslmode=") != std::string::npos) ci += " sslmode=require";`
- **Bukti**: (a) `sslmode=disable` pada PG lokal no-TLS → dipaksa `require` → koneksi GAGAL senyap pada DB sehat; (b) `sslmode=verify-full` → diturunkan ke `require` → verifikasi cert/hostname hilang (MITM tak terdeteksi); `connect_timeout`/`application_name`/`sslrootcert` diabaikan.
- **Skenario**: operator set verify-full untuk kepatuhan → sistem diam-diam berjalan tanpa verifikasi cert.
- **Remediasi**: parse query-string jadi pasangan key=value utuh; default `require` hanya jika absent.

### I2 — **[CLOSED — P34]** `limit_req` login menghantam GET

- **Kategori**: konfigurasi / DoS-ringan
- **Lokasi**: `nginx.conf:75-88` — zone rate-limit login diterapkan pada semua method termasuk GET.
- **Skenario**: attacker menyapu halaman login (GET) dari satu IP → semua user di belakang NAT/kuota IP sama terkunci; atau sebaliknya GET membuat noise yang menghabiskan kuota rate-limit sehingga POST login sah ikut ditolak.
- **Remediasi**: batasi zone ke `location` + `limit_except POST` (atau pisahkan zone POST).

### I3 — **[CLOSED — P34]** `.env` masuk build context Docker

- **Kategori**: keamanan / supply-chain
- **Lokasi**: `.env` di root repo ikut terkirim ke build context (tidak dikecualikan).
- **Bukti**: file berisi kredensial riil (secret, admin pass, DB/Redis password, kredensial R2 — nilai TIDAK dicantumkan di dokumen ini).
- **Skenario**: image/layer bocor → seluruh kredensial produksi ikut.
- **Remediasi**: `.dockerignore` + pindahkan secret ke runtime env/secret manager; rotasi kredensial yang pernah masuk context.

### I4 — (MEDIUM, detail tipis) **[OPEN — verifikasi ulang]**

Judul/label dari laporan agent I; detail bukti tidak selamat kompaksi konteks. Verifikasi ulang saat remediasi (kandidat: konfigurasi compose/Dockerfile — lihat area I).

### I5 — Endpoint Health "live" tanpa probe dependency (merge temuan mini-review Health) **[OPEN]**

- **Kategori**: observability
- **Lokasi**: endpoint health menyatakan "live" tanpa memeriksa PG/Redis; plus dead-code health terkait (bagian LOW).
- **Skenario**: PG down → health tetap hijau → orchestrator/proxy tidak mengalihkan trafik; operator tidak tahu ada outage.
- **Remediasi**: `live` = probe proses; tambah `ready` = cek PG+Redis (fail → 503); jangan gabungkan jadi satu status boolean.

### F2 — Parser needle no-space vs writer **[OPEN]**

- **Kategori**: kontrak parser
- **Lokasi**: parser needle no-space `src/api/exams.cpp:470-494` (`:489` collapse whitespace, `:490` `"required":true`); writer `src/admin/exams.cpp:1095-1154` (`:1100`, `:1148`).
- **Bukti**: writer menghasilkan format yang parser tongol/bahkan tidak cocok needle-nya → round-trip putus di edge-case whitespace.
- **Remediasi**: samakan needle/writer (satu helper serialization); test round-trip property.

### F3 — Token mentah di 302 **[OPEN]**

- **Kategori**: keamanan / kebocoran token
- **Lokasi**: `src/public/hasil.cpp:399-404` — token hasil ujian disisipkan mentah ke Location redirect.
- **Skenario**: token terekspose di header/proxy log/Referer.
- **Remediasi**: gunakan session/lookup pendek, atau minimal setel header no-referrer di endpoint.

### F5 — Stack pool di api/webhook/hasil (anggota keluarga stack-pool) **[CLOSED — P34]**

- **Kategori**: perf / koneksi
- **Lokasi**: `src/api/exams.cpp:402-403`, `:419-420`; `src/api/webhook.cpp:145`; `src/public/hasil.cpp:86`, `:457`, `:539`. (Call-site di luar scope F yang satu kelas: `admin/export.cpp:168`, `auth_store.cpp` ×6.)
- **Remediasi**: konversi ke `global_pool()`; satu remediasi keluarga (lihat Dedup).

### F6 — `g_idem_jobs` statis: growth-only, tidak pernah dibersihkan **[OPEN]**

- **Kategori**: memory / idempotency
- **Lokasi**: `src/api/exams.cpp:45-46` (`static g_idem_mu/g_idem_jobs`), `:960-999` (hanya insert, tak pernah hapus), `:360-383` (Redis INCR sebagian).
- **Bukti**: map tumbuh tanpa batas per key unik → memory leak lambat di process berumur panjang. BERBEDA dari D1 (D1 = reservation bocor di jalur protobuf; ini = pertumbuhan struktur).
- **Remediasi**: TTL/eviction (LRU atau expiry per entri) + recycling key completed.

### F7 — **[CLOSED — P34]** `r2.enabled()` tanpa cek bucket; presign bucket kosong

- **Kategori**: konfigurasi / validasi
- **Lokasi**: `src/storage/r2.hpp:19` (`enabled()` tak memeriksa bucket), `src/storage/r2.cpp:41-48` (presign `""` empty-bucket, `:47` endpoint check).
- **Skenario**: R2 "enabled" dengan bucket kosong → presign URL ke bucket "" → error runtime aneh/kegagalan diam di jalur upload PDF.
- **Remediasi**: `enabled()` = access+secret+bucket+endpoint lengkap; fail-closed + log saat konfigurasi parsial.

### F9 — Keluarga stack-pool (catatan kelas) **[CLOSED — P34]**

Kelas sistemik yang mewakili D5/E-c/G1/G9/F5 — lihat bagian Dedup untuk anggota lengkap + remediasi tunggal.

---

## Temuan LOW (32)

> **Status Batch 3: OPEN** — seluruh LOW belum diremediasi, KECUALI anggota keluarga stack-pool (D5, G9) yang tertutup oleh remediasi tunggal Batch 2 (P34).

### Area D (9)

- **D5** — **[CLOSED — P34]** stack-local `DbPool pool(db_url,2); RealPool real(...,2);` di `src/admin/exams.cpp:50-51, 1039-1040, 1164-1165, 1219-1220 (per-exam dalam loop bulk!), 1366-1367, 1451-1452`; `pengawas.cpp:124-125`. T2 hanya mengonversi 5 duplikat with_pg (`pengawas.cpp:72-74` terverifikasi). Anggota keluarga stack-pool.
- **D6** — `wib_to_utc_iso` sscanf 5 field tanpa `%n` full-consumption, range hanya `d<=31` (`exams.cpp:946-960`): `"2026-05-01 10:00JUNK"` diterima; `"2026-02-31"` menjadi 3 Maret.
- **D7** — audit-log residual: `create_exam`, `update_exam` (toggle/start/stop/edit/edit-token/regenerate), `delete_exam` (`:875-915`), `delegate_exam` (`:1431-1508`), `save_exam_questions` (`:1093-1194`) tanpa `write_audit_log`; hanya `bulk_toggle:1284`, `bulk_delete:1332`, `set_approval:406`, `set_auto_approve:474` yang punya. Gap B1 pass-20 lebih luas.
- **D8** — UPDATE 0-baris tetap 200 di `set_approval` (`:398-408`)/`set_auto_approve` (`:467-475`): `PGRES_COMMAND_OK` true untuk 0-row UPDATE. Fix: `atoi(PQcmdTuples(up.get())) > 0` else 404.
- **D9** — stub `pengawas_state` hardcoded `{"connected":true,"active_exams":0,...}` (`pengawas.cpp:514-517`, route `router_full.cpp:451`); frontend mem-poll-nya (`uiux-batch6-jscore.test.mjs:320-328`).
- **D10** — komentar `exam_bulk_scope_ok` klaim fail-open (`:1205-1206`) tapi kode fail-closed (`:1216-1217` `if(db_url.empty())` skip PG → return false). Fix: one-line comment. (`test_p30_tdd.cpp` mengunci substring `exam_bulk_scope_ok` — aman.)
- **D11** — `user_id=0` disimpan sebagai 0, bukan NULL (`exams.cpp:54-56`, `pengawas.cpp:371-373`): `NULLIF($2,'')::int` dengan `std::to_string(user_id)` → `"0"` bukan `""`. Fix: `user_id>0 ? std::to_string(user_id) : ""`.
- **D12** — filter dashboard mengecualikan pengawas yang ditugaskan via `exam_pengawas` (`dashboard.cpp:96, 138`): predikat `super_admin || created_by==actor_id || delegated_to==actor_id` tidak mencakup exam_pengawas, padahal `exam_access` router dan M8 bulk menerima mereka → pengawas bisa membuka submissions X tapi dashboard kosong.
- **D13** — misc dashboard: `submissions:0` hardcoded (`dashboard.cpp:155` protobuf, `:164` JSON); heuristik replace empty-state raw (`:99-111`, cari `"<div class=\"empty-state\">"` lalu `"</p>"`+`"</div>"` pertama); `storage_mb` integer floor (`:146`); fallback no-PG hardcode `"is_privileged":true` (`:236`); `get_auto_approve` exam hilang → 200 enabled:false, bukan 404 (`:447-449`).

### Area E (5)

- **E-b** — dead code: `export_submissions_csv` hardcoded, belum di-wire.
- **E-d** — churn: early-return tanpa `release()` menghancurkan koneksi sehat (`PgConnDeleter=PQfinish`) di `system_apps_page` error paths, `submissions.cpp:212-217/296-301`.
- **E-i / E-j / E-k** — LOW (label dari laporan agent E; detail tidak selamat kompaksi — verifikasi ulang saat remediasi). Terkait: pass ini juga mencatat **E-e RESOLVED — bukan temuan** (dianulir oleh agent E sendiri).

### Lain-lain (4)

- **Health dead-code** — kode health mati sisa refactor (pendamping I5).
- **`submissions.cpp:304`** — `PQresultErrorMessage` berpotensi NULL saat `PQresultStatus` non-error path (defensif; klasifikasi libpq-contract).
- **Version-gate no-op** — gate versi yang tidak melakukan apa pun (mini-review version-gate).
- **Dead middleware** — middleware version yang mati/tidak terpakai (satu area dengan version-gate no-op; lihat `src/middleware/version.cpp`).

### Area G (6)

- **G5** — `stop()` sleep backoff per-job di final drain (`submission_queue.cpp:361-363`): 40 job × 5s = 200s shutdown. Fix: max-backoff sekali seperti `run_batch:463-466`.
- **G6** — AUTH tanpa username + `redisConnect` tanpa timeout (`redis_real.cpp:42`, `:46`): Redis managed ACL non-default user → WRONGPASS senyap. Fix: `AUTH %s %s` (username,password) Redis 6+ + `redisConnectWithTimeout`/`redisSetTimeout`.
- **G7** — `drain_heartbeat_batch` BEGIN tak diperiksa (`:540`): BEGIN gagal → 500 INSERT di koneksi rusak, semua requeue, 500× log error. Fix: `if(!real.exec_params(conn,"BEGIN",{})) { requeue batch; return 0; }`.
- **G8** — `enqueue_job_to_redis` koneksi Redis baru per submit (`api/exams.cpp:139-151`) — jalur terpanas. Fix: thread-local/pool (pola `queue_redis` thread_local `main.cpp:127-134` sudah ada).
- **G9** — **[CLOSED — P34]** `main.cpp:43` stack `RealPool db` (migrate/hydrate) terpisah dari `global_pool()` → dua pool process-wide, budget koneksi dobel, idle selamanya. Anggota keluarga stack-pool. Diperbaiki: `main.cpp` kini menginisialisasi `global_pool()` lebih awal dan memakainya untuk migrate/hydrate; status koneksi startup via `pool->ping()`.
- **G10** — test hygiene `tests/test_pg_integration_tdd.cpp`: cleanup manual non-RAII (`:456-459/:474-477/:524-526/:578-580` DELETE manual setelah ASSERT), mutasi `saas_settings` tanpa restore (`:366-371` — `email_verification_enabled`/`turnstile_enabled` dipaksa '0' permanen), `DEL` seluruh queue (`:581-583` menghapus heartbeat proses lain). ASSERT gagal mid-test → kontaminasi DB dev/CI bersama. Fix: guard RAII (pola `CreateExam:492-502`), snapshot+restore saas_settings, payload marker unik bukan global DEL.

### Area I (5)

- **I6–I10** — LOW (label dari laporan agent I; detail tidak selamat kompaksi — verifikasi ulang saat remediasi; kandidat area: ssl/HSTS, upstream go_backend `down`, map $uri $backend, header nginx).

### Area F (3)

- **F8** — R2 HEAD-request: curl hang 15 detik (`r2.cpp:283-296`).
- **F10** — OR-gate token/approved: `if(!examtoken::matches(...) && !device_approved(exam_id, sub_mac))` (`api/exams.cpp:904`) — submit lolos TANPA token valid jika MAC approved; MAC diambil dari body client. Paritas dengan backend Go perlu dicek.
- **F11** — `sanitize_mac` (`api/exams.cpp:1162-1171`) + unknown-bucket sharing & raw XFF (label; detail parsial).

---

## Dedup Lintas Agent

1. **Keluarga stack-local pool = SATU KELAS** — anggota: D5 (`admin/exams.cpp` ×6 + `pengawas.cpp:124-125`), E-c (`export.cpp:166-168`), G1 (`submission_queue.cpp:409-411`), G9 (`main.cpp:43`), F5 (`api/exams.cpp:402-403/:419-420`, `webhook.cpp:145`, `public/hasil.cpp:86/457/539`), F9 (catatan kelas), plus `check_auth` per-request `RealPool` (`router_full.cpp:326-350`) dan call-site di `auth_store.cpp` ×6 / `admin/export.cpp:168`. **Remediasi tunggal**: konversi semua ke `global_pool()`/`with_global_pg` + perluas ban literal P29 (`RealPool real(`) dan P32 ke `submission_queue.cpp`, `router_full.cpp`, `export.cpp`, `hasil.cpp`, `api/exams.cpp`, `webhook.cpp`, `auth_store.cpp`, `main.cpp`.
2. **Health live-no-probe ↔ I5 merged** — satu temuan.
3. **D1 ≠ F6** — D1 = reservation idempotency bocor di jalur error protobuf; F6 = pertumbuhan map `g_idem_jobs` tanpa eviksi. Terpisah, keduanya tetap dilaporkan.
4. **Kelas fake-200 admin write path** — E-a, E-g, E-h, D2 membentuk satu kelas sistemik M9-residual; remediasi sebaiknya satu batch + satu kontrak test fail-closed admin-write generik.

## Terverifikasi Baik

### Dari agent D (admin exams) — 6

1. **XSS SSR tertutup penuh** — semua interpolasi HTML di `build_exam_table_html` (`dashboard.cpp:17-80`) teks DAN atribut (data-name, title, aria-label, data-token, data-exam-name) via `html_escape` (`sanitize.cpp:33-45`, escape `& < > " '`); audit JSON via `json_escape_pw` (`pengawas.cpp:48-65`).
2. **Privilege gating router lengkap** — semua route per-exam (`router_full.cpp:428-458`) digating "exam"/"exam_access" (superadmin | created_by | delegated_to | exam_pengawas | operator same-instansi); paritas bulk via `exam_bulk_scope_ok` dikunci test_p30 M8.
3. **M9 fail-closed benar & konsisten** di `set_approval` (`:411-418`→503), `get_auto_approve` (`:443-447`), `set_auto_approve` (`:478-484`), `delete_submission`.
4. **B1 atribusi audit benar** (`pengawas.cpp:363-376`, NULLIF terparameter; header internal diinjeksi router bukan client; best-effort).
5. **SQL terparameter menyeluruh**, termasuk subquery scoping dinamis (`pengawas.cpp:147-159`).
6. **Kontrak idempotency jalur form + paritas cleanup** di `create_exam` (release di semua jalur error form, unclaim token `:445/:463/:497`, orphan-cleanup R2 `:498-510`, finalize `:530-537`).

### Dari agent G (queue/store/db/redis/utils/models) — 6

1. **P24 heartbeat-flusher CONFIRMED CLOSED** — `drain_heartbeats_once` (`:596-615`) kini `global_pool()` + cek `PQstatus` + release benar; dikunci P32 (FlusherUsesGlobalPool, FlusherKeepsConnectionsHot, NoDetachInQueueSources).
2. **Integritas batch worker** — SAVEPOINT per-job + `pg_advisory_xact_lock(exam+mac)` + `UPDATE...RETURNING` + fallback `INSERT ON CONFLICT`; retry-exhausted → `kFailedQueueKey` (T5) dengan JobResult.
3. **T1 fresh-DB schema** cross-verified vs INSERT riil (submissions 10 kolom, student_access_logs 9 kolom + ON CONFLICT DO NOTHING, admin_users 20 kolom ALTER).
4. **Desain `test_pg_integration` kuat** — Group A isolasi via temporary CREATE DATABASE per run; parser SQL-ekstraksi menangani escape `\"`/`\\`/`\n` + substitusi `$n`; auto-SKIP tanpa DATABASE_URL/PG/Redis; kontrak non-PG tetap jalan (redeem BEGIN+FOR UPDATE+COMMIT).
5. **M9 (P26) + C5 (P27) konsisten** — 503 saat DATABASE_URL aktif + DB down (mode memory 200); posix_threads joinable + notify_all + join saat stop, tanpa `.detach()`; startup fail-closed HAS_LIBPQ tanpa DB → exit 1 kecuali EXAMVAN_ALLOW_MEMORY_STORE=1.
6. **Paritas store memory vs PG**, `retry_backoff_ms` (P17-L2) benar, `set_lpush_checked` (P18-C4) → 503 saat LPUSH gagal, versi 2.7.2 satu sumber `config.hpp` (T4).

### Dari kill-notifikasi H/J/K + pengawas (verifikasi parsial sebelum agent dihentikan)

- **H**: `custom_token` divalidasi ketat `is_valid_exam_token` + panjang tepat 8 + uppercase (`exams.cpp:414-424`), charset A-Z0-9 → `javascript:` href mustahil. `template_helper.cpp:7-27` hanya mengganti `{{.version}}` (+CSRF `:29-90`); `pengawas_detail.html:1094` `var EXAM_ID = {{.e...}}`.
- **J**: `apiFetch` melampirkan `X-CSRF-Token` dari meta untuk POST/PUT/DELETE/PATCH (admin-core.js L78-83).
- **K**: `is_valid_exam_token` (`utils.cpp:62-65`) whitelist alfanumerik 6-32 kuat; `login.cpp:48-50` prioritas admin→public login.rendered.html; `login.cpp:80` set cookie `__Host-csrf_token` di production.
- **Pengawas (parsial)**: `escapeHtml` kedua tanda kutip; `apiFetch` CSRF; `showToast`; `formatDateTimeID` raw-on-parse-fail. `pengawas.cpp:89-99` tanpa substitusi field.
- **Dari F (parsial)**: gate order exam_pdf 4-layer benar; rate-limit sebelum token gate; `device_approved` fail-closed.

### Keraguan terdokumentasi (dari G — bukan temuan, dicatat untuk pass berikutnya)

1. `suspended_by_cascade` (`models/user.hpp:37`) field mati.
2. `reserve_idempotency` mengembalikan `{Conflict}` saat acquire koneksi gagal (`exam_store_postgres.cpp:414-452`, caller `admin/exams.cpp:270`) — 409 misleading saat DB down post-startup; tidak dievaluasi karena startup sudah fail-closed.
3. Needle `finalize_idempotency` `"\"id\":"` (`:454-490`) + fallback `next_id()` `return 1` (`:362-367`) rapuh, tanpa skenario kegagalan konkret.
4. D1-D13 di luar scope G, dicatat untuk pass ini (sudah di atas).

---

## Remediasi Prioritas

**Batch 1 — HIGH (fake-200 family + keamanan)** — ✅ SELESAI (P33, 2026-09-10)

1. E-a/E-g/E-h/D2: fail-closed semua jalur tulis admin (503 saat DB down). Kontrak: perluas pola M9 ke test generik admin-write.
2. E-f: expiry voucher di SQL / parse ketat fail-closed + test string `timestamptz` riil.
3. F1: OTP expiry+attempts, ack setelah validasi, route ber-rate-limit. **Amandemen**: `Webhook_ValidProtobufResponse` (`tests/test_protobuf_handlers_tdd.cpp:737`) — pindahkan assert ack ke jalur valid. `Webhook_PgPinsActivationSql` (`:2010`) aman.
4. F4: whitelist/escape `X-Version` (test `:1854` hanya mengunci forwarding — aman).
5. I1: samakan `client_max_body_size` system-apps.

**Batch 2 — MEDIUM (durability + koneksi)** — ✅ SELESAI (P34/P35, 2026-09-10; D3+D4 via P35)

6. G2: cek `requeue()` → push_failed+JobResult (hook tersedia).
7. Keluarga stack-pool (D5/E-c/G1/G9/F5 + router/auth_store): konversi `global_pool()` + ban P29/P32.
8. G3: PQconnectdb di luar mutex + connect_timeout.
9. G4: parse sslmode utuh (jangan paksa require).
10. D1: `release_and_return` di 3 jalur protobuf + perluas `IdempotencyReleaseOnFailure`.
11. D3: claim-token atomik. D4: urutan store→R2 + hapus objek lama. F7: `enabled()` cek lengkap. I2: limit_req POST-only. I3: `.dockerignore` + rotasi secret.

**Batch 3 — LOW/higienis** — OPEN (belum dimulai)

12. D6/D8/D10/D11: fix mikro. D7: audit-log lengkap. D12: predikat dashboard + exam_pengawas.
13. G5-G9, F8/F10, E-b/E-d: sesuai deskripsi masing-masing.
14. **Higienis**: commit `tests/test_p32_tdd.cpp` + `tests/test_pg_integration_tdd.cpp` + 6 file worker-tree bersama remediasi (kelas-T8). ✅ Terpenuhi oleh commit remediasi ini — seluruh worker tree + test P32..P35 ikut di-commit bersama.

## Rekomendasi Testing

- Kontrak fail-closed admin-write generik (E-a/E-g/E-h/D2) — satu fixture DB-down dipakai semua handler tulis.
- Test durabilitas queue: kill Redis di antara LPUSH → assert job ada di failed-queue/JobResult (G2).
- Ban-test literal perluasan (stack-pool): grep-ban `RealPool real(` + `DbPool pool(` di file yang belum dikunci.
- Round-trip test writer↔parser (F2).
- Test konfigurasi: `nginx -t` + upload >5MB e2e (I1); unit `pg_conninfo_from_url` untuk sslmode utuh (G4).
- `test_pg_integration` Group B: RAII guard + restore saas_settings + payload marker unik (G10).

## Catatan Cakupan

- **H (frontend JS), J (admin templates), K (public templates)** dihentikan di tengah review atas permintaan user; verifikasi parsial (kill-notification) masuk bagian Terverifikasi Baik. Review template/JS lengkap ditunda ke pass berikutnya jika diperlukan.
- **Agent pengawas** mati 429 — cakupannya disubsumsi scope H.
- **Agent server-core** mati 429 — cakupannya (main/server/config/hub/socketio/jobs/version middleware) diverifikasi mandiri oleh G di area queue/pool/main (laporan G menyatakan self-verified penuh).
- Item detail-tipis (E-h/E-i/E-j/E-k, I4, I6-I10, F11) tercatat label+severity; bukti lengkap tidak selamat kompaksi konteks sesi — verifikasi ulang file terkait sebelum remediasi. (Catatan remediasi: E-h ternyata cukup buktinya dan CLOSED via P33; E-i/E-j/E-k, I4, I6-I10, F11 tetap OPEN — verifikasi ulang.)
- Skipped test `P7_Frontend.JsGuardCount` — kondisi sama dengan pass sebelumnya, bukan regresi baru.
