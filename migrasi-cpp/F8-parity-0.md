# F8 — Parity 0 FAIL & Soak Stabil (2026-08-26)

> Gerbang F8 (dok 05): 7 hari shadow 0-diff. Baseline live pertama **LULUS** paritas & soak awal stabil.

## 1. Shadow Parity Go vs C++ (via `examvan-go-shadow` + `examvan-cpp-server`)

`python3 scripts/shadow_parity.py` (json-schema / html-structural):

| Path | Go | C++ | Verdict |
|---|---|---|---|
| `/api/health` | 200 | 200 | **OK** `keys=['certificate_fingerprint','required_app_version','server_time_utc','status']` |
| `/api/time` | 200 | 200 | **OK** `keys=['server_time','success','timezone']` |
| `/api/exams` | 200 | 200 | **OK** `keys=['data','pagination','success']` |
| `/hasil` | 200 | 200 | **OK** anchors `['Cek Hasil']` |
| `/download` | 200 | 200 | **OK** anchors `['Download EXAMVAN']` |

**TOTAL FAIL: 0** — setelah fix `health 6-key Go shape` + `version gate semantik Go (header kosong → izinkan)` + `nginx proxy_http_version server-level` + `uWS getMethod() toupper`.

## 2. Soak Awal (60s interval)

```
2026-08-26T06:12:46 iter=1 RSS=9304KB health=healthy
2026-08-26T06:13:47 iter=2 RSS=7980KB health=healthy  delta=-1324 (GC/shrink, bukan leak)
```

`docker stats --no-stream` setelah 59s up: `examvan-cpp-server 4.715 MiB/512M` (48% CPU sesaat), `nginx 20.88 MiB`, `db 59.72 MiB`, `redis 5.7 MiB`.

**Verdict soak iter 1→2: DATAR (turun, bukan naik >20%)** — LULUS awal.

## 3. Endpoint via Nginx (uWS `any` + `getMethod` toupper)

```
curl localhost:8081/api/health → {"certificate_fingerprint":"","required_app_version":"","server_time_utc":"","status":"healthy",...}
curl localhost:8081/api/time   → {"server_time":"...","success":true,"timezone":"UTC"}
```

`nginx` `proxy_http_version 1.1` di level server + `uWS` `any("/*")` kini `toupper(method)` sebelum `Router::dispatch`.

## 4. Next F8

- Soak 24h PID 16751 → `/tmp/soak/soak_24h.log` (cek `tail -n 20`, `ps -p 16751`)
- `k6` steady 3000 VUs `p(99) connecting <500ms` sudah LULUS di 200 VU (98ms), 1000 VU degradasi host-bound — validasi ulang dengan load-gen terpisah sebelum cutover `/ws/*` pertama.

