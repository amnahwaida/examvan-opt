# F8 — Dual-Run Shadow → Cutover 7 Hari (Gerbang F8)

> Dok 05: Dua deployment independen, nginx `map per-grup`, kriteria 7 hari hijau, rollback <1m.

## 1. Kriteria Gerbang (wajib 7 hari berturut)

| # | Kriteria | Bukti | Status |
|---|---|---|---|
| 1 | Paritas 0 diff (json-schema/html-structural/byte-exact) | `scripts/parity_harness.py --go http://examvan-go-f0:5000 --cpp http://webui-cpp:5000` + `shadow_proxy.py` live | ⏳ shadow 0-diff 7 hari |
| 2 | p99 ≤ target F0 (WS <500ms, submit <800ms) | `k6 --out json` `p(99)` | ⏳ |
| 3 | RSS datar (tanpa leak) | `soak_check.sh` 24h `ps -o rss` | ⏳ Go 36→43 MiB, C++ 4.8→5.2 MiB (F0) |
| 4 | Error < baseline+0.1% | `docker logs` + `nginx access.log` | ⏳ |
| 5 | Soak 24h staging | `docker compose up -d` 24h | ⏳ |

## 2. Urutan Cutover (dok 05 §3)

1. `~^/ws/.*` → `cpp_backend` (F3, dampak terbesar)
2. `/api/health`, `/api/time` → `cpp`
3. `/hasil`, `/download` (html-structural)
4. `/login`, `/admin/dashboard` (SSR)
5. CRUD ` /admin/api/users`, `/admin/api/exams` + R2 upload
6. Export XLSX + jobs (SETNX `job:*`)

## 3. Shadow Proxy

```bash
python3 scripts/shadow_proxy.py --go http://examvan-go-f0:5000 --cpp http://webui-cpp:5000 --port 8080
# duplikat request, bandingkan paritas, log diff → dashboard
```

`parity_harness.py` dry-run:
```bash
python3 scripts/parity_harness.py --go http://localhost:5001 --cpp http://localhost:8081
# /api/health: Go=200 Cpp=200 OK, /api/time: OK, etc. 0 diff = gerbang lulus
```

## 4. Rollback (<1m)

```bash
sed -i 's/cpp_backend/go_backend/' nginx/nginx.conf  # per-grup atau global
docker exec examvan-cpp-nginx nginx -s reload
# sesi dual-key tetap valid, skema tidak berubah, queue prefix terpisah
```

## 5. Dashboard

- `docker stats --no-stream` RSS tiap 60s → `soak_check.sh` log
- `k6 --out json` p99 → `F0-baseline.md` target
- `parity_harness` cron tiap 5m → `scripts/fixtures/golden/` diff

## 6. Verdict

**BELUM LULUS F8** — perlu 7 hari shadow 0-diff + soak 24h. Next: jalankan `shadow_proxy` live vs Go bench (sudah `examvan-go:bench` image), isi `F8-baseline-live.md`.
