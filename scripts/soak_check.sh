#!/bin/bash
# Soak 24h RSS check (dok 04 Lapis 3): grafik RSS datar, p99 dalam target F0
set -e
PID=${1:-$(pgrep examvan-server)}
if [ -z "$PID" ]; then echo "examvan-server not running"; exit 1; fi
echo "Monitoring RSS for PID $PID every 60s (24h)..."
for i in $(seq 1 1440); do
  RSS=$(ps -o rss= -p $PID | tr -d ' ')
  echo "$(date -Iseconds) RSS=${RSS}KB"
  sleep 60
done
