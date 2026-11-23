import json
import sys

r = json.load(sys.stdin)
print("partial:", r.get("partial"), "| offcpu_total_ms:", r["offcpu_total_ns"] / 1e6,
      "| unattributed:", len(r.get("unattributed_events") or []))
for s in r.get("spans", []):
    print("  %-14s dur=%7.2fms offcpu=%7.2fms self=%7.2fms events=%d" % (
        s["name"], s["duration_ns"] / 1e6, s["offcpu_ns"] / 1e6, s["self_offcpu_ns"] / 1e6, len(s.get("events") or [])))
for c in r["causes"][:3]:
    print("  cause:", " <- ".join(c["path"]), "| blocked_in:", c["blocked_in"], "| root:", c["root_cause"],
          "| %.2f ms" % (c["total_ns"] / 1e6))
