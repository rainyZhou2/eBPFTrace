#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

ch() { docker compose exec -T clickhouse clickhouse-client --user etrace --password etrace -q "$1"; }

docker compose down -v >/dev/null 2>&1 || true
docker compose up -d >/dev/null
trap 'docker compose down -v >/dev/null 2>&1' EXIT

for _ in $(seq 1 60); do
  n=$(ch "SELECT count() FROM etrace.offcpu_events WHERE trace_id != '' AND start_time < now() - INTERVAL 5 SECOND" 2>/dev/null || echo 0)
  [ "${n:-0}" -gt 5 ] && break
  sleep 2
done

trace=$(ch "SELECT trace_id FROM etrace.offcpu_events WHERE trace_id != '' AND start_time < now() - INTERVAL 5 SECOND ORDER BY start_time DESC LIMIT 1")
[ -n "$trace" ] || { echo "FAIL: no traced off-CPU events"; exit 1; }

report=$(curl -fsS "http://127.0.0.1:18080/api/v1/traces/${trace}/offcpu")
echo "$report" | python3 report.py
echo "$report" | python3 -c '
import json, sys
r = json.load(sys.stdin)
assert not r.get("partial"), r.get("partial")
spans = {s["name"]: s for s in r["spans"]}
lc, root = spans["load_cache"], spans["GET /orders"]
assert lc["self_offcpu_ns"] > 20e6, "load_cache should carry the lock wait"
assert root["offcpu_ns"] >= lc["self_offcpu_ns"], "root must include child off-CPU time"
assert any(len(c["path"]) >= 3 for c in r["causes"]), "expected a multi-hop cause"
print("PASS")
'
