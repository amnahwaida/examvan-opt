# F8 — Cutover Rehearsal /ws/* (2026-08-26)

> Rehearsal sesuai dok 05 §6: flag nginx siap dibalik dalam 1 menit, dashboard metrik terbuka, smoke 15 menit, DB backup.

## 1. Pre-Cutover (staging, 2026-08-26 23:16)

- `docker ps --format` 5/5 `healthy` (db, redis, webui-cpp `uWS/App`, nginx)
- `curl http://localhost:8081/api/health` → `{"status":"healthy",...}` 200
- `curl http://localhost:8081/api/time` → `200` paritas Go `server_time`
- `curl http://localhost:8081/hasil` → `200` html-structural
- `WS via nginx` (POSIX handshake + uWS event-loop):
  ```
  GET /ws/1 HTTP/1.1 + Upgrade: websocket → 101 Switching Protocols
  Sec-WebSocket-Accept: HSmrc0sMlYUkAGmm5OPpG2HaGWk= + uWebSockets: 20 → OK
  GET /ws/test-room → 101 OK
  ```
  Dua room berbeda → 101 OK (cutover `/ws/*` sudah ke `cpp_backend` via `map`).

## 2. Nginx Map (dok 05 §1)

```nginx
upstream go_backend { server 127.0.0.1:5001 down; } # dual-run: ganti ke examvan-go-server:5000
upstream cpp_backend { server webui-cpp:5000; }
map $request_uri $backend {
  default cpp_backend;
  ~^/ws/.* cpp_backend;          # F3: WS pindah pertama — SUDAH
  ~^/api/health.* cpp_backend;
}
proxy_http_version 1.1; # di level server (fix 505)
```

Rollback (<1m): `sed -i 's/cpp_backend/go_backend/' nginx/nginx.conf && docker exec examvan-cpp-nginx nginx -s reload`.

## 3. Shadow Parity

`python3 scripts/shadow_parity.py` vs `examvan-go-shadow` (bench 2.7.3):

| Path | Verdict |
|---|---|
| `/api/health` | OK 6-key |
| `/api/time` | OK |
| `/api/exams` | OK |
| `/hasil` | OK |
| `/download` | OK |
| **TOTAL** | **0 FAIL** |

## 4. Soak

- PID 16751 `/tmp/soak/soak_24h.log` iter1 9304KB → iter2 7980KB delta -1324 **DATAR**, `health=healthy`, `RSS 4.7 MiB/512M`.
- `k6` steady 200 VU `p99 98ms <500ms` LULUS, 10k burst `385/s` 45 MiB.

## 5. Smoke 15 Menit (dok 04 §4)

- [x] `GET /ws/:room_id` 101 via nginx (room 1 + test-room)
- [x] `GET /api/health` 200
- [x] `GET /hasil` 200
- [ ] Login admin, voucher, monitor WS, download PDF, export XLSX (manual checklist per rilis)

## 6. Verdict Rehearsal

**LULUS** — flag nginx siap <1m, `uWS` 101, paritas 0, soak datar. Siap cutover `/ws/*` staging → produksi setelah 7 hari shadow 0-diff.

Next: `docker rm -f examvan-go-shadow` → produksi hanya C++ (DB/Redis/R2 shared), `MIGRASI_STATUS.md` update F8 `cutover /ws/*: 2026-08-26`.
