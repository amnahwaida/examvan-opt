# examvan-opt — C++20/uWebSockets Port of EXAMVAN

> **Migrasi Go → C++20** (Opsi B hybrid → penggantian penuh). Dokumen perencanaan di `migrasi-cpp/` — baca `00-ringkasan-eksekutif.md` dulu.

## Status Fase (F0–F8)

| Fase | Isi | Status |
|---|---|---|
| F0 | Profiling RSS/latency WS (RAM/koneksi) | ✅ `k6_ws.js` target ≤100MB/10k |
| F1 | Kontrak dibekukan (40+ route + WS) | ✅ `contract.json` + `parity_harness.py` |
| F2 | Skeleton C++: CMake, CI+ASan, Docker, PG/Redis pool | ✅ 75 tests hijau |
| F3 | WebSocket hub (room/heartbeat/privileged/backpressure) | ✅ port `hub.go` + uWS adapter stub |
| F4 | Read-only HTTP (hasil, cek_hasil, download) | ✅ `handlers/public` |
| F5 | Sesi dual-key HMAC + CSRF + Turnstile + version gate | ✅ `session/cookie` dual + `scoring` |
| F6 | Admin CRUD + R2 presign SigV4 + XLSX | ✅ `handlers/admin/*` + `handlers/r2` |
| F7 | Queue submission + jobs SETNX | ✅ `queue/submission_queue` + `jobs` |
| F8 | Dual-run shadow + cutover per-grup | ✅ `nginx map` + `shadow_proxy.py` + `docs-cutover.md` |

## Quick Start

```bash
cp .env.example .env   # isi R2 + secrets (wajib, fail-fast)
docker compose up -d --build
curl http://localhost:8081/api/health
```

Dev:
```bash
cmake -B build && cmake --build build -j && ./build/examvan-tests  # 75 OK
cmake -B build-san -DENABLE_SANITIZERS=ON && cmake --build build-san -j && ./build-san/examvan-tests
./build/examvan-server  # ENV real, 65 routes, uWS stub → production WITH_UWEBSOCKETS=ON
for f in static/js/*.test.mjs; do node --test "$f"; done  # guard 68 files
k6 run --env WS_URL=ws://localhost:8081/ws/1 scripts/load_test/k6_ws.js
python3 scripts/parity_harness.py --go http://go:5000 --cpp http://cpp:5000
python3 scripts/shadow_proxy.py --go http://go:5000 --cpp http://cpp:5000 --port 8080
```

## Arsitektur

```
[EXAMVAN Go]  ─┐
               ├── nginx `map $request_uri $backend` per-grup ─► APK/desktop (kontrak beku)
[webui-cpp uWS] ─┘
         └─► PostgreSQL + Redis + R2 (SAMA, skema tidak berubah, skema.sql referensi)
```

Sesi `examvan_session` HMAC-SHA256 dual-key (current+previous) lintas proses.

## Pemetaan & Standar

| Go | C++ |
|---|---|
| Gin | `Router` + `router_full` (uWS `App`) |
| gorilla/websocket 594b | `Hub`/`Client` + `server/server` adapter |
| pgx v5 | `DbPool` + `db/pool_real` libpq (HAS_LIBPQ) |
| go-redis v9 | `RedisClient` + `redis_real` hiredis (HAS_HIREDIS) |
| sessions securecookie | OpenSSL HMAC dual-key |
| excelize | `export` CSV+XLSX PK placeholder → OpenXLSX |
| Turnstile | libcurl (HAS_LIBCURL) |

C++20, GCC13+, `-Wall -Wextra -Wpedantic -Werror`, ASan+UBSan, `clang-format`, `new/delete` dilarang.

## Struktur

```
src/{config,models,helpers,scoring,services/examtoken,handlers/{public,api,admin,r2},middleware,websocket,queue,jobs,server,db,redis}
tests/ 75 unit (sanitize,socketio,session,hub,config,csrf,handlers,models,middleware,api,public,r2,queue,export,characterization,scoring,dual)
scripts/{extract_contract.py,parity_harness.py,shadow_proxy.py,soak_check.sh,load_test/k6_ws.js}
templates/ static/ (68 guard JS, dark-by-design)
```

## Metodologi TEST-FIRST

Kontrak MERAH dari output Go → HIJAU; `shadow_proxy` diff paritas permanen.

## Dokumen

`migrasi-cpp/00` ringkasan, `01` fase, `02` risiko, `03` kontrak, `04` pengujian, `05` rollback, `docs-cutover.md` runbook.
