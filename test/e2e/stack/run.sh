#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

ch() { docker compose exec -T clickhouse clickhouse-client --user etrace --password etrace -q "$1"; }

export COMPOSE_PROJECT_NAME="etrace-e2e-$$"
trap 'docker compose down -v >/dev/null 2>&1' EXIT
docker compose up -d --build >/dev/null

check_scenario() {
  local scenario=$1 waiter=$2 trace="" report
  for _ in $(seq 1 60); do
    if [ -z "$trace" ]; then
      trace=$(ch "SELECT trace_id FROM etrace.offcpu_events
        WHERE trace_id != '' AND comm = '$waiter' AND reason = 'blocked' AND duration_ns >= 20000000
        ORDER BY start_time DESC LIMIT 1" 2>/dev/null || true)
    fi
    if [ -n "$trace" ] && report=$(curl -fsS --max-time 5 "http://127.0.0.1:18080/api/v1/traces/${trace}/offcpu" 2>/dev/null) &&
      python3 -c 'import json, sys
names = {span["name"] for span in json.load(sys.stdin).get("spans", [])}
sys.exit(not {"GET /orders", "load_cache"}.issubset(names))' <<< "$report"; then
      python3 report.py "$scenario" <<< "$report"
      return
    fi
    sleep 2
  done
  echo "FAIL: traced lock wait for $scenario did not reach ClickHouse and Jaeger (trace: ${trace:-none})" >&2
  docker compose logs --tail=40 agent app query >&2
  return 1
}

check_scenario handoff lc_handoff
check_scenario overlap lc_overlap
