#!/bin/bash
# F0: run k6 via docker (tanpa install host)
docker run --rm -i --network examvan-opt_internal grafana/k6 run -e WS_URL=ws://webui-cpp:5000/ws/1 - < scripts/load_test/k6_ws.js
