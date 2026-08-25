# F8 — Decommission Go (Setelah 7 Hari Hijau, Dok 05 §5)

> Hanya jalankan setelah 7 hari berturut `F8 dashboard` hijau: paritas 0, p99 <500ms, RSS datar, error <0.1%.

## 1. Matikan Traffic Go (tetap deploy 2 minggu)

```bash
# Nginx sudah map semua ke cpp_backend (rehearsal 2026-08-26). Verifikasi:
grep "cpp_backend" nginx/nginx.conf
docker exec examvan-cpp-nginx nginx -T | grep -A2 "map"
# Go shadow sudah removed (docker rm -f examvan-go-shadow), produksi 4 kontainer.
docker ps --format '{{.Names}} {{.Status}}' | grep examvan
# Harus: examvan-cpp-{db,redis,server,nginx} 4/4 healthy, tanpa examvan-go-*
```

- Jangan hapus `examvan-go:bench` image (rollback cepat).
- `soak_check.sh` tetap jalan PID 16751 → `tail -n 20 /tmp/soak/soak_24h.log` harus `DATAR`.

## 2. Arsip Go

```bash
cd /home/vannyezha/project/sekolah/EXAMVAN
git tag -a v-go-final -m "Arsip Go sebelum decommission C++ (migrasi F8)"
git push origin v-go-final
# Simpan dump schema.sql + data anonim seed untuk staging C++ jika perlu
pg_dump -h examvan-cpp-db -U examvan examvan > /tmp/examvan-final.sql
```

## 3. Pindahkan Ownership Job & Runbook

- Jobs `expiry`/`approval_cleanup`/`access_log_retention` sudah `SETNX job:*` di C++ (`src/jobs/jobs.cpp` + `redis/client.cpp`).
- Pastikan `crontab`/`systemd timer` yang dulu trigger job Go dimatikan; C++ `JobRunner` (expiry 3600s, cleanup 1800s, retention 86400s) yang aktif.

## 4. Hapus Route Go dari Nginx (hari ke-14)

```bash
# nginx.conf: hapus upstream go_backend (down) dan map fallback
# Sebelum:
#   upstream go_backend { server 127.0.0.1:5001 down; }
# Sesudah:
#   # Go decommissioned 2026-09-09 — hanya cpp_backend
#   upstream cpp_backend { server webui-cpp:5000; }
#   map $request_uri $backend { default cpp_backend; }
docker compose restart nginx
curl -f http://localhost:8081/api/health  # {"status":"healthy",...}
```

## 5. Verifikasi Akhir

```bash
bash scripts/f8_dashboard.sh
# Harus: RSS DATAR, 4/4 healthy, parity 0 (tanpa Go shadow, cek golden terakhir), k6 200 VU p95 <500ms 100% pong
for f in static/js/*.test.mjs; do node --test "$f" > /dev/null || echo "FAIL $f"; done && echo "68 guard ALL PASSED"
./build/examvan-tests --gtest_filter=-ServerLive.*:F7Jobs.JobRunnerStartStop 2>&1 | grep PASSED
```

## 6. Checklist Hari-Decommission (per grup, dok 05 §6)

- [ ] Flag nginx rollback <1m rehearsal sudah (2026-08-26 WS 101 OK)
- [ ] Dashboard metrik (RSS/p99/error) terbuka
- [ ] Smoke 15m (login admin, voucher, monitor WS, download, export XLSX)
- [ ] DB backup terbaru restore-tested (`/tmp/examvan-final.sql`)
- [ ] Jendela di luar jam ujian

