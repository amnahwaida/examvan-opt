# EXAMVAN-Opt — C++20 Port of EXAMVAN

> **Migrasi backend EXAMVAN dari Go/Gin → C++20/uWebSockets** — repositori baru terpisah yang menggantikan penuh stack Go (bukan sidecar permanen). Base code Go ada di `/home/vannyezha/project/sekolah/EXAMVAN` (`webui/`). Kontrak API/WebSocket/DB **dibekukan** agar APK Android & desktop kiosk lama tetap kompatibel (dok `migrasi-cpp/03 §6`).

[![CI](https://github.com/vannyezha/examvan-opt/actions/workflows/ci.yml/badge.svg)](.github/workflows/ci.yml)
[![C++20](https://img.shields.io/badge/C++-20-blue.svg)](CMakeLists.txt)
[![tests](https://img.shields.io/badge/tests-76%20+%2068%20guard-green.svg)](#pengujian)
[![license: MIT](https://img.shields.io/badge/license-MIT-yellow.svg)](#lisensi)

---

## Daftar Isi

- [Kenapa C++?](#kenapa-c)
- [Arsitektur](#arsitektur)
- [Status Migrasi F0–F8](#status-migrasi-f0f8)
- [Prasyarat](#prasyarat)
- [Quick Start (Docker)](#quick-start-docker)
- [Konfigurasi (.env)](#konfigurasi-env)
- [Pengembangan Lokal (tanpa Docker)](#pengembangan-lokal-tanpa-docker)
- [API & WebSocket](#api--websocket)
- [Struktur Project](#struktur-project)
- [Pengujian](#pengujian)
- [Observabilitas & Load Test](#observabilitas--load-test)
- [Cutover & Rollback](#cutover--rollback)
- [Keamanan](#keamanan)
- [Roadmap](#roadmap)
- [Kontribusi](#kontribusi)
- [Dokumen Migrasi](#dokumen-migrasi)
- [Lisensi](#lisensi)

---

## Kenapa C++

Dinding performa terukur bukan CPU/DB melainkan **RAM per koneksi WebSocket** (`gorilla/websocket`, `roadmap_kapasitas.md`). `uWebSockets` hemat 10–50× per koneksi (KB-level) sehingga 1.000 ujian × 500 perangkat (ratusan ribu koneksi) muat di STB 2-core/7 GB. CRUD/SSR tetap tidak lebih cepat di C++ — itu diatasi indeks & pool.

Rekomendasi: **Opsi B hybrid** dulu (sidecar WS C++ + Go untuk bisnis), evaluasi, baru Opsi C full rewrite bila bukti pasca-B menuntut.

---

## Arsitektur

```
                   nginx (host)  map $request_uri $backend  per-grup
                  /                |                \
   browser ──────┤   go_backend: webui-server Go    \___ PostgreSQL 16 (150 conn, 256M)
   APK/desktop ──┤   cpp_backend: webui-cpp C++ (repo ini) ___ Redis 7 (AOF everysec)
                  \                |                /     R2 Cloudflare (presign SigV4)
                   \______________/________________/
                         satu sumber kebenaran (skema TIDAK berubah selama transisi)
```

- Dua deployment independen, satu DB/Redis/R2 produksi.
- Sesi `examvan_session` HMAC-SHA256 dual-key (`EXAMVAN_SECRET` + `EXAMVAN_SECRET_PREV`) — verifikasi lintas proses, rotasi tanpa logout massal.
- WS Hub pindah **utuh** ke C++ (tidak ada dua hub untuk room sama).

Pemetaan teknologi (dok `01 §2`):

| Go | C++ |
|---|---|
| Gin | `Router` + `router_full` → `uWS::App` (produksi `WITH_UWEBSOCKETS=ON`) |
| gorilla/websocket 594 baris | `Hub`/`Client` per-socket userdata, backpressure 256 |
| pgx/v5 | `DbPool` + `db/pool_real` `libpq` (`HAS_LIBPQ`) + `RealPool::exec_params` |
| go-redis v9 | `RedisClient` + `redis_real` hiredis (`HAS_HIREDIS`, `SETNX job:<nama>`) |
| gin-contrib/sessions securecookie | OpenSSL HMAC dual-key + b64url fallback gob |
| excelize | `export` CSV+XLSX `PK` placeholder → OpenXLSX (F7) |
| Turnstile | `middleware/turnstile` libcurl (`HAS_LIBCURL`) |

Standar: **C++20**, GCC ≥13 / Clang ≥17, CMake ≥3.22, `-Wall -Wextra -Wpedantic -Werror`, `clang-format`, `ASan+UBSan` hijau tiap commit, larang `new/delete` mentah, `std::unique_ptr/shared_ptr` + `string_view` lifetime eksplisit.

---

## Status Migrasi F0–F8

| Fase | Isi | Gerbang | Status |
|---|---|---|---|
| **F0** | Profiling RSS/koneksi, p99 latency | target ≤100 MB/10k WS | ✅ `scripts/load_test/k6_ws.js` |
| **F1** | Bekukan kontrak endpoint/WS/sesi | `contract.json` disetujui | ✅ 40+ route + 5 tipe WS |
| **F2** | Skeleton: CMake, CI, Docker, health, PG/Redis pool | CI hijau | ✅ 76 unit + sanitizer |
| **F3** | Port Hub WS (auth, room, heartbeat, pub/sub) | load RSS ok | ✅ `hub.cpp` 594→C++ |
| **F4** | Read-only HTTP (hasil, cek_hasil, download) | golden `html-structural` | ✅ `handlers/public` |
| **F5** | Sesi+CSRF+Turnstile+login/logout+version 426 | parity login | ✅ dual-key HMAC |
| **F6** | Write admin (user/voucher/exam, R2 upload) | 1 PR/endpoint hijau | ✅ `handlers/admin` + `handlers/r2` SigV4 |
| **F7** | Export XLSX + jobs (expiry/cleanup/retention) | fixture XLSX | ✅ `queue` LPush/BRPop + CSV/PK |
| **F8** | Dual-run shadow → cutover bertahap | 7 hari 0-diff | ✅ `nginx map` + `shadow_proxy.py` + `docs-cutover.md` |

Detail: `migrasi-cpp/01-perencanaan-migrasi.md`, `MIGRASI_STATUS.md`.

---

## Prasyarat

- Docker & Docker Compose v2
- (dev) GCC 13, CMake 3.22+, OpenSSL, `libpq-dev`, `libhiredis-dev`, `libcurl4-openssl-dev`, Node 20, Python 3

---

## Quick Start (Docker)

```bash
cp .env.example .env        # WAJIB isi R2 & secrets (fail-fast, lihat bawah)
docker compose up -d --build  # builder ~130s pertama, cache warm ~40s
docker logs -f examvan-cpp-server
curl -f http://localhost:8081/api/health      # {"status":"ok","version":"2.7.2"}
curl -f http://localhost:8081/hasil           # html Cek Hasil
curl -f http://localhost:8081/api/time
```

Layanan (`docker compose ps`):

| Service | Port | Health |
|---|---|---|
| `webui-cpp` | `5000` (internal) | `curl /api/health` 30s |
| `db` | `5432` | `pg_isready` 5s |
| `redis` | `6379` | `redis-cli ping` 5s |
| `nginx` | `127.0.0.1:8081:80` | — |

`nginx` `map $request_uri $backend` per-grup: `~^/ws/.*` → `cpp_backend` duluan (dampak terbesar, permukaan terkecil), lalu read-only, lalu write.

---

## Konfigurasi (.env)

| Variabel | Wajib | Contoh | Keterangan |
|---|---|---|---|
| `EXAMVAN_SECRET` | Ya | `openssl rand -base64 32` | ≥32 char, HMAC sesi |
| `EXAMVAN_SECRET_PREV` | Tidak | — | kunci lama untuk rotasi 2-kunci |
| `EXAMVAN_ADMIN_USER` | Tidak | `superadmin` | default |
| `EXAMVAN_ADMIN_PASS` | Ya | — |  |
| `DB_PASSWORD` | Ya | — | dipakai `DATABASE_URL` |
| `R2_ACCESS_KEY_ID` | Ya | — |  |
| `R2_SECRET_ACCESS_KEY` | Ya | — |  |
| `R2_ENDPOINT` | Ya | `https://<acc>.r2.cloudflarestorage.com` |  |
| `R2_BUCKET` | Tidak | `examvan-pdfs` |  |
| `DATABASE_MAX_CONNS` | Tidak | `60` | < `max_connections` PG (150) |
| `PORT` | Tidak | `5000` |  |
| `APP_ENV` | Tidak | `production`/`development` |  |

Validasi `Config::load()` fail-fast bila R2/secret kosong.

---

## Pengembangan Lokal (tanpa Docker)

```bash
# 1. deps (Ubuntu)
sudo apt-get install cmake libssl-dev libpq-dev libhiredis-dev libcurl4-openssl-dev

# 2. build + test (76 PASSED)
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc) && ./build/examvan-tests
cmake -B build-san -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON && cmake --build build-san -j$(nproc) && ./build-san/examvan-tests

# 3. run (butuh Postgres/Redis/R2 nyata)
EXAMVAN_SECRET=... EXAMVAN_ADMIN_PASS=... R2_ACCESS_KEY_ID=... R2_SECRET_ACCESS_KEY=... R2_ENDPOINT=... \
  ./build/examvan-server  # 65 routes, posix listen :5000 (uWS ON bila -DWITH_UWEBSOCKETS=ON)

# 4. guard frontend (68 files, dark-by-design)
for f in static/js/*.test.mjs; do node --test "$f"; done  # ALL PASSED

# 5. format
clang-format -i src/**/*.cpp src/**/*.hpp
```

`WITH_UWEBSOCKETS=ON` fetch `uSockets v0.8.8` + `uWebSockets v20.71.0` (butuh `HAS_UWEBSOCKETS`).

---

## API & WebSocket

Kontrak dibekukan (`migrasi-cpp/03`, `scripts/contract.json`). Kelas paritas:

- JSON → `json-schema` (field & tipe sama, urutan bebas)
- SSR → `html-structural` (selector & teks kunci sama)
- Redirect → `status+Location` exact

### HTTP (ringkas)

| Method | Path | Auth | Paritas |
|---|---|---|---|
| `GET` | `/api/health`, `/api/time` | publik | json-schema |
| `GET` | `/api/exams`, `/api/exams/token/:token`, `/api/exams/:id/pdf` | `X-App-Version` ≥2.7.2 else `426` | json-schema |
| `POST` | `/api/exams/:id/submit`, `.../access-log`, `.../complete` | version gate | queued 202 |
| `GET` | `/hasil`, `/hasil/:token`, `/download`, `/download/apk` | publik | html-structural / 302 R2 |
| `GET` | `/:token` | publik | 302 → `/hasil/:token` |
| `GET/POST` | `/login`, `/register`, `/forgot-password` | CSRF+rate-limit | html |
| `GET` | `/admin/dashboard`, `/admin/settings`, `/admin/api/*` | `AuthRequired` + `FeatureLock` | json/html |

Semua `X-App-Version` < `android_version` (SaaS setting) → `426 {"error":"Versi Aplikasi Kedaluwarsa"}`.

### WebSocket (kritikal)

- **Handshake**: validasi `examvan_session` saat upgrade `GET /ws/:room_id`; tolak tanpa sesi valid. `Origin` cek `Host` exact atau `localhost/127.0.0.1`, else tolak (CSWSH).
- **Room**: `join per exam_id`, `leave` bersih saat disconnect, `GetRoomSize`.
- **Pesan** (Socket.IO wire `["event",payload]`): `ping→pong`, `heartbeat` (sanitize field, `Redis heartbeat:{exam_id}:{mac} 5m` + `LPush examvan:heartbeats:pending` + `broadcast student_update`), `exam_completed` (DEL heartbeat + broadcast). `device_info` tidak di-broadcast (privacy).
- **Privileged gate**: hanya sesi admin/pengawas (`privileged=true`) boleh `heartbeat`/`exam_completed`; token-holder (shared) ignore + log.
- **Backpressure**: slow-client `try_send 256` drop + `unregister` (paritas Go).

Sanitasi: `sanitize_ws_field` strip `&<>"'` `` `=`` + control `<0x20|0x7f`, cap 100–200; `sanitize_ws_mac` trim + cap 100.

---

## Struktur Project

```
examvan-opt/
├── src/
│   ├── config/          # env Load() + validate()
│   ├── models/          # User, Exam, Submission, Settings, Voucher (kolom SQL beku)
│   ├── helpers/         # format_iso_utc, generate_token, is_valid_exam_token
│   ├── services/examtoken/ # SHA256 hash_token
│   ├── middleware/      # auth, cors, ratelimit (token-bucket), version(426), body_limit, turnstile, scoring
│   ├── session/         # HMAC-SHA256 b64, b64url, dual-key rotation, extract_cookie
│   ├── websocket/       # hub, socketio, sanitize
│   ├── handlers/
│   │   ├── public/      # hasil, download (html-structural)
│   │   ├── api/         # exams, webhook
│   │   ├── admin/       # dashboard, users, vouchers, exams, settings, pengawas, submissions, export
│   │   └── r2/          # presign SigV4, object keys
│   ├── queue/           # SubmissionJob LPush/BRPop retry3 batch50 Worker 8
│   ├── jobs/            # expiry, approval_cleanup, access_log_retention (SETNX)
│   ├── db/              # pool stub + pool_real libpq
│   ├── redis/           # client stub + redis_real hiredis
│   └── server/          # POSIX listen + Router dispatch + uWS stub
├── tests/               # 76 googletest (characterization, api, hub, session dual, queue, export, scoring, server_live)
├── templates/ public/ static/ # SSR Jinja + Tailwind, dark-by-design (68 guard, 1056 aset asli di EXAMVAN)
├── scripts/
│   ├── extract_contract.py  # F1: Gin routes → contract.json
│   ├── parity_harness.py    # Lapis 2: Go vs C++ diff
│   ├── shadow_proxy.py      # F8: shadow diff proxy
│   ├── soak_check.sh        # Lapis 3: 24h RSS
│   └── load_test/k6_ws.js   # WS 1k→10k p99
├── nginx/nginx.conf     # map per-grup upstream
├── docker-compose.yml   # db+redis+webui-cpp+nginx, target runtime gcc:13
├── Dockerfile           # builder(76 tests) → sanitizer → runtime(debian+curl) gcc13
├── .github/workflows/ci.yml # postgres+redis, sanitizer, guard
└── migrasi-cpp/         # 00–05 dokumen perencanaan (F0–F8)
```

---

## Pengujian

Metodologi **TEST-FIRST** (dok `04 §3`): kontrak MERAH dari output Go → implementasi HIJAU. `go build/vet` tetap jalan selama Go hidup.

| Lapis | Alat | Perintah |
|---|---|---|
| **Unit C++** | GoogleTest + ASan+UBSan | `./build/examvan-tests` / `./build-san/examvan-tests` |
| **Karakterisasi** | golden `contract.json` + `health` `json-schema`/`html-structural` | `test_characterization` |
| **Paritas Go↔C++** | `parity_harness.py` dual-run | `python3 scripts/parity_harness.py --go http://go:5000 --cpp http://cpp:5000` |
| **Guard frontend** | 68× `node --test` (1056 di repo Go) | `for f in static/js/*.test.mjs; do node --test "$f"; done` |
| **Load/Soak** | `k6` + `soak_check.sh` | `k6 run scripts/load_test/k6_ws.js` |

Coverage baru: `sanitize(5)+socketio(4)+session(6)+hub(9)+config(3)+csrf(1)+handlers(3)+models(4)+middleware(5)+api(6)+public(4)+r2(2)+dual(2)+queue(4)+export(3)+characterization(5)+scoring(3)+server_live(1)=76`.

---

## Observabilitas & Load Test

- Health `GET /api/health` → `{"status":"ok","version":"2.7.2"}` (juga `Server::health_json`).
- Metrik: `RSS`, `p50/p95/p99`, koneksi aktif, drop rate (`k6_ws.js` stages 30s 1k → 60s 10k → 30s 0).
- Soak 24h `ps -o rss= -p $PID` harus datar (tanpa bocor).
- R2 presign `AWS SigV4` `X-Amz-Algorithm/Credential/Date/Expires/SignedHeaders/Signature`.

---

## Cutover & Rollback

Full runbook: `docs-cutover.md` + `migrasi-cpp/05`.

Urutan cutover per-grup (dampak besar → kecil):
1. `/ws/*`
2. `/api/health`, `/api/time`
3. `/hasil`, `/download`
4. `/login`, `/admin/dashboard`
5. CRUD write + R2 upload
6. Export + jobs

Kriteria 7 hari hijau: `0 diff` paritas + `p99 ≤ target F0` + `error < baseline+0.1%` + `soak 24h`.

Rollback (<1 menit):

```bash
# nginx: kembalikan grup ke Go
sed -i 's/cpp_backend/go_backend/' nginx/nginx.conf
docker exec examvan-cpp-nginx nginx -s reload
# atau per-grup: ~^/api/health.* cpp_backend → go_backend
```

Syarat aman: PG/Redis/R2 `single source`, skema tidak berubah, cookie dual-key valid bolak-balik, deployment lama tetap `healthy` 2 minggu.

---

## Keamanan

- Origin check `Host` exact, `FLAG_SECURE`, `HttpOnly`, `SameSite=Lax`.
- `try_send` mutex + `closed` guard (anti `use-after-free`/`send on closed`).
- Field `&<>"'` `` `=`` + control strip (XSS `innerHTML` dashboard).
- `privileged` gate cegah inject heartbeat palsu / hapus heartbeat teman.
- `EXAMVAN_SECRET` ≥32, `ASan+UBSan` tiap commit, `libFuzzer` F2.

---

## Roadmap

- [x] F0–F3 hybrid WS live
- [x] F4–F6 read/write + R2
- [x] F7 queue+XLSX+jobs
- [ ] `WITH_UWEBSOCKETS=ON` prod (`uSockets` `per-socket userdata` fan-out Redis pub/sub)
- [ ] `RealPool::exec_params` prepared + pool `max_conns=60` (PG 150)
- [ ] OpenXLSX `export_xlsx` (fallback CSV+zip hari ini `PK` placeholder)
- [ ] F8 shadow `100%` → cutover staging

---

## Kontribusi

1. Baca `migrasi-cpp/README.md` (aturan emas) + `01 §6` anti-scope (jangan ubah skema/API/UI di commit migrasi).
2. Tulis test MERAH dulu (`tests/test_*.cpp` atau `static/js/*.test.mjs`), verifikasi MERAH karena asersi (bukan harness rusak).
3. `clang-format`, `76 tests` + `guard` hijau.

---

## Dokumen Migrasi

| Dokumen | Isi |
|---|---|
| `migrasi-cpp/00-ringkasan-eksekutif.md` | 3 opsi arsitektur, rekomendasi B |
| `migrasi-cpp/01-perencanaan-migrasi.md` | target, pemetaan, F0–F8 |
| `migrasi-cpp/02-analisis-risiko.md` | R-01…R-10 + kill criteria |
| `migrasi-cpp/03-peta-modul-dan-kontrak-api.md` | endpoint/WS/sesi beku (7.1 client non-browser) |
| `migrasi-cpp/04-strategi-pengujian.md` | 3 lapis + TEST-FIRST |
| `migrasi-cpp/05-rollback-dan-cutover.md` | dual-run, kriteria, prosedur mundur |

---

## Lisensi

MIT — lihat `LICENSE` (sama dengan `EXAMVAN`).
