# Cutover Runbook (dok 05 implementasi)

## Dua Deployment Independen
- Go: `EXAMVAN` (webui-server:5000) — dibekukan
- C++: `webui-cpp:5000` — repo ini
- Satu PG/Redis/R2 produksi, `webui-cpp` connect via DATABASE_URL/REDIS_URL yang sama
- Nginx host `map $request_uri $backend` per-grup (nginx/nginx.conf)

## Urutan Cutover (dok 05 §3)
1. /ws/* (F3) — pindah Hub C++ dulu, observasi RSS/p99
2. /api/health, /api/time, /api/exams read-only
3. /hasil, /download (public SSR)
4. admin SSR + login/logout
5. CRUD write + R2 upload
6. Export XLSX + jobs (SETNX job:<nama>)

## Kriteria 7 hari hijau per grup (dok 05 §2)
- Parity 0 diff (scripts/parity_harness.py + shadow_proxy.py)
- p99 ≤ target F0, RSS datar (scripts/soak_check.sh)
- error < baseline+0.1%, soak 24h staging

## Rollback (<1 menit)
```bash
# nginx: ubah map line grup ke go_backend dan reload
sed -i 's/cpp_backend/go_backend/' nginx/nginx.conf && docker exec nginx nginx -s reload
```
- Cookie sesi dual-key (current+previous) tetap valid bolak-balik
- Skema tidak berubah, R2 keys identik, queue prefix terpisah saat shadow

## Verifikasi
- `curl /api/health` → {"status":"ok"}
- `k6 run scripts/load_test/k6_ws.js` — 10k WS ≤ target RSS
- `python3 scripts/shadow_proxy.py --go http://go:5000 --cpp http://cpp:5000`
