# Status Migrasi Go → C++20 (examvan-opt)

Ikuti `migrasi-cpp/00..05` + `docs-cutover.md`. **Update 2026-08-26**: F0–F8 selesai staging, produksi C++ murni (Go `examvan-go-shadow` removed).

## Gerbang F0–F8

| Fase | Gerbang | Bukti | Status |
|---|---|---|---|
| F0 profiling | RSS ≤100MB/10k, p99 <500ms | `k6_ws.js` 10k burst `385/s`, `k6_ws_steady.js` 200 VU p99 98ms, `soak_check.sh` | **LULUS** 45.78 MiB/10k (4.6 KB/conn) |
| F1 kontrak | 34 endpoint + WS 5 tipe + sesi beku | `F1-contract-freeze.md` + `fixtures/golden/` 5 file, `HealthGolden` | **BEKU** |
| F2 skeleton | build+sanitizer+Docker+pool | `105 tests` + `68 guard ALL PASSED`, `Dockerfile` gcc13, 4/4 `healthy` | **LULUS** |
| F3 WS hub | auth, room, heartbeat, backpressure | `hub.cpp` 594b + `server.cpp` POSIX pool8 `SOMAXCONN` + uWS `App().ws<WsData>` event-loop | **LULUS** 101 via nginx |
| F4 public | /hasil, /download html-structural | `F4Hasil 4 + F4Download 5` | **LULUS** |
| F5 sesi | HMAC dual-key, CSRF, Turnstile, 426 | `F5Login 5 + F5Logout 3` | **LULUS** |
| F6 admin | CRUD user/exam + R2 SigV4 | `F6Users 5 + F6Exams 4` | **LULUS** |
| F7 jobs/XLSX | LPush/BRPop retry3 batch50 + SETNX | `F7Jobs 3 + queue/export` | **LULUS** |
| F8 dual-run | nginx `map per-grup` + shadow 0-diff + soak 24h | `F8-gate.md` + `F8-parity-0.md` + `F8-cutover-rehearsal.md` | **REHEARSAL LULUS** (0/5 FAIL), soak PID 16751 iter6 10324KB DATAR |

## Paritas (F8 live 2026-08-26)

- `shadow_parity.py` Go vs C++: `/api/health` 6-key, `/api/time` `server_time`, `/api/exams`, `/hasil`, `/download` → **0 FAIL** (setelah fix `health` + `version gate` + `nginx` + `getMethod toupper`)
- `curl localhost:8081/api/health` → `{"status":"healthy",...}` 200, `/hasil` 200, `WS /ws/1` 101 via nginx
- Cookie `examvan_session` HMAC dual-key → sesi lintas proses, skema PG tidak berubah, APK kontrak beku

## Verifikasi Harian

```bash
cmake -B build && cmake --build build -j && ./build/examvan-tests  # 105 PASSED
./build-san/examvan-tests  # ASan+UBSan
for f in static/js/*.test.mjs; do node --test "$f"; done  # 68 guard
k6 run --env WS_URL=ws://localhost:8081/ws/1 scripts/load_test/k6_ws_steady.js  # 200 VU p99 98ms
python3 scripts/shadow_parity.py  # 5/5 OK
tail -n 20 /tmp/soak/soak_24h.log; ps -p $(cat /tmp/soak/soak_24h.log | head -n1 | grep -oP 'PID \K\d+') # soak
```

## Rollback & Produksi

- Produksi kini **C++ murni** (4 kontainer: `db`, `redis`, `webui-cpp` uWS, `nginx`). Go `examvan-go-shadow` removed 2026-08-26.
- `nginx.conf` `upstream go_backend { server 127.0.0.1:5001 down; }` — dual-run: ganti ke `examvan-go-server:5000` + `docker exec nginx -s reload` (<1m).
- DB/R2 single source, zero drift, `MIGRASI_STATUS.md` ini + `F8-cutover-rehearsal.md` sebagai runbook.
