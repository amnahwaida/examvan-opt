#!/bin/bash
# F8 dashboard harian (dok 05 §2): 0-diff + p99 + RSS + error <0.1% — harus hijau 7 hari berturut sebelum cutover final.
set -u
LOG_DIR=${LOG_DIR:-/tmp/soak}
SOAK_LOG=${SOAK_LOG:-/tmp/soak/soak_24h.log}
RESULT=/tmp/f8-dashboard-$(date +%Y%m%d).log

echo "=== F8 DASHBOARD $(date -Iseconds) ===" | tee "$RESULT"
echo "PID soak: $(ps -p 16751 -o etime= 2>/dev/null || echo 'GONE') | $(tail -n 1 $SOAK_LOG 2>&1 | head)" | tee -a "$RESULT"
echo "---docker ps---" | tee -a "$RESULT"
docker ps --format '{{.Names}} {{.Status}}' | grep examvan | tee -a "$RESULT"
echo "---docker stats (1 sample)---" | tee -a "$RESULT"
docker stats --no-stream --format '{{.Name}} {{.MemUsage}} {{.CPUPerc}}' | grep examvan | tee -a "$RESULT"
echo "---health via nginx---" | tee -a "$RESULT"
curl -s http://localhost:8081/api/health | head -c 120 | tee -a "$RESULT"; echo "" | tee -a "$RESULT"
echo "---parity (Go shadow vs C++)---" | tee -a "$RESULT"
# Go shadow sudah removed produksi — untuk F8 7-hari, jalankan kembali shadow sementara:
# docker run -d --network examvan-opt_internal --name examvan-go-shadow ... (lihat F1) lalu:
if docker ps --format '{{.Names}}' | grep -q examvan-go-shadow; then
  python3 /home/vannyezha/project/sekolah/examvan-opt/scripts/shadow_parity.py 2>&1 | tee -a "$RESULT"
else
  echo "shadow: Go not running (produksi C++ murni) — untuk paritas harian: docker run examvan-go:bench --network examvan-opt_internal ..." | tee -a "$RESULT"
  echo "cek parity terakhir: $(cat /home/vannyezha/project/sekolah/examvan-opt/migrasi-cpp/F8-parity-0.md | grep -m1 'TOTAL')" | tee -a "$RESULT"
fi
echo "---k6 steady sample (200 VU, 20s hold)---" | tee -a "$RESULT"
timeout 40 docker run --rm --network examvan-opt_internal -v $PWD/scripts/load_test/k6_ws_steady.js:/k6.js grafana/k6 run -e WS_URL=ws://webui-cpp:5000/ws/1 -e TARGET_VUS=200 -e HOLD_MS=5000 /k6.js 2>&1 | grep -E "ws_connecting|checks_" | tail -n 5 | tee -a "$RESULT"
echo "=== END $(date -Iseconds) log=$RESULT ===" | tee -a "$RESULT"
cat "$RESULT"
