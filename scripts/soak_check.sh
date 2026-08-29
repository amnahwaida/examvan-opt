#!/bin/bash
# Soak 24h RSS p99 monitor (dok 04 Lapis 3): grafik RSS harus datar (tanpa leak).
# Usage: ./soak_check.sh <container_name> [output_log]
# Robust: resolve PID server asli tiap iterasi (tahan terhadap restart proses
# di dalam container), catat health status, dan deteksi tren kenaikan.
set -u

CONTAINER=${1:-examvan-cpp-server}
LOG=${2:-/tmp/soak_$(date +%Y%m%d_%H%M%S).log}
INTERVAL=${INTERVAL:-60}
ITERATIONS=${ITERATIONS:-1440}   # 1440 x 60s = 24 jam

log() { echo "$(date -Iseconds) $*" | tee -a "$LOG"; }

log "=== SOAK START container=$CONTAINER interval=${INTERVAL}s iterations=$ITERATIONS ==="

FIRST_RSS=""
LAST_RSS=""
RESTARTS=0
PREV_PID=""

for i in $(seq 1 "$ITERATIONS"); do
  # Health status container menurut Docker
  HEALTH=$(docker inspect --format '{{if .State.Health}}{{.State.Health.Status}}{{else}}none{{end}}' "$CONTAINER" 2>/dev/null || echo "GONE")

  # PID server asli (bukan docker-init): cari proses yang exec binary-nya
  PID=$(docker top "$CONTAINER" -o pid,cmd 2>/dev/null | grep 'examvan-server' | grep -v 'docker-init\|grep' | awk '{print $1}' | head -n1)

  if [ -z "$PID" ]; then
    log "iter=$i ERROR: proses examvan-server tidak ditemukan (health=$HEALTH)"
    sleep "$INTERVAL"
    continue
  fi

  # Deteksi restart proses (PID berubah)
  if [ -n "$PREV_PID" ] && [ "$PID" != "$PREV_PID" ]; then
    RESTARTS=$((RESTARTS+1))
    log "iter=$i WARN: PID berubah $PREV_PID -> $PID (restart ke-$RESTARTS)"
  fi
  PREV_PID="$PID"

  # RSS proses server dalam KB
  RSS=$(ps -o rss= -p "$PID" 2>/dev/null | tr -d ' ')
  if [ -z "$RSS" ]; then
    log "iter=$i ERROR: gagal baca RSS pid=$PID"
    sleep "$INTERVAL"
    continue
  fi

  [ -z "$FIRST_RSS" ] && FIRST_RSS="$RSS"
  LAST_RSS="$RSS"

  # CPU % sesaat sebagai konteks (opsional, tidak wajib)
  CPU=$(ps -o %cpu= -p "$PID" 2>/dev/null | tr -d ' ')

  log "iter=$i RSS=${RSS}KB cpu=${CPU}% health=$HEALTH"
  sleep "$INTERVAL"
done

# ===== Ringkasan =====
log "=== SOAK END ==="
if [ -n "$FIRST_RSS" ] && [ -n "$LAST_RSS" ]; then
  DIFF=$((LAST_RSS - FIRST_RSS))
  log "RSS awal=${FIRST_RSS}KB akhir=${LAST_RSS}KB delta=${DIFF}KB restarts=$RESTARTS"
  # Ambang leak: naik >20% dari baseline dianggap indikasi bocor (dok 04: grafik datar)
  THRESH=$(( FIRST_RSS * 120 / 100 ))
  if [ "$LAST_RSS" -gt "$THRESH" ]; then
    log "VERDICT: INDICASI LEAK (akhir > 120% awal)"
    exit 2
  else
    log "VERDICT: DATAR / LULUS (delta <= 20%)"
    exit 0
  fi
else
  log "VERDICT: TIDAK ADA DATA"
  exit 1
fi
