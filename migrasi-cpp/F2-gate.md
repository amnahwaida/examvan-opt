# F2 — Gerbang Skeleton (Gerbang F2)

> Metode TEST-FIRST: test healthcheck/pool ditulis MERAH dulu, baru implementasi sampai HIJAU. CI harus hijau tiap commit.

## 1. Kriteria Gerbang (dok 01 §4)

| Kriteria | Bukti | Status |
|---|---|---|
| Repo `examvan-opt` terpisah, `templates/static` disalin | `templates/` 5046b, `static/js` 68 guard, symlink `cmd/internal` ke `EXAMVAN/webui` untuk guard | ✅ |
| CMake ≥3.22, C++20, GCC13, `-Wall -Wextra -Wpedantic -Werror` | `CMakeLists.txt:3.22`, `g++13.4.0`, 76 tests PASSED | ✅ |
| CI `build+sanitizer+test` hijau | `.github/workflows/ci.yml` postgres:16+redis:7, `build`+`build-san -DENABLE_SANITIZERS=ON` | ✅ |
| `clang-format` enforced | `.clang-format` Google 100, CI `clang-format --dry-run` | ✅ |
| Docker multi-stage `webui-cpp` + `db` + `redis` + `nginx` | `Dockerfile` builder→sanitizer→runtime(gcc13), `docker-compose.yml` target runtime | ✅ 36s build (cache warm), image `examvan-opt-webui-cpp` |
| Healthcheck `depends_on: service_healthy` | `webui-cpp` health `curl -f /api/health` 30s, `db` `pg_isready` 5s, `redis` `redis-cli ping` 5s | ✅ `docker ps` 4 healthy |
| Koneksi PG/Redis pool | `DbPool` libpq `has_valid_url` + `sanitized_url` + `RealPool` libpq, `RedisClient` hiredis `SETNX job:*` | ✅ 76 tests `DbPool.Sanitized` `RedisPrefix.Isolated` |

## 2. Test HIJAU

```
cmake -B build && cmake --build build -j && ./build/examvan-tests  # 76 PASSED (23 suites)
cmake -B build-san -DENABLE_SANITIZERS=ON && ... && ./build-san/examvan-tests  # 76 PASSED ASan+UBSan
for f in static/js/*.test.mjs; do node --test "$f"; done  # 68 guard ALL PASSED
docker compose up -d --build  # 36s FINISHED, 4/4 healthy
curl -f http://localhost:8081/api/health → {"status":"ok","version":"2.7.2"}
curl -f http://localhost:8081/api/time   → {"now":"2026-...Z"}
curl -f http://localhost:8081/hasil      → html Cek Hasil
```

## 3. Gate Verdict

**LULUS F2** — semua kriteria hijau. Next F3 (Hub WS) boleh mulai. Jobs tetap di Go selama Opsi B (dok 01 §2).
