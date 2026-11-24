import json
import sys

SCENARIOS = {"handoff": ("lc_handoff", "lc_release"), "overlap": ("lc_overlap", "lc_decoy")}


def check(scenario, report):
    waiter, signaler = SCENARIOS[scenario]
    assert not report.get("partial"), report.get("partial")
    spans = {span["name"]: span for span in report["spans"]}
    cache, root = spans["load_cache"], spans["GET /orders"]
    assert cache["parent_span_id"] == root["span_id"], "load_cache must be a direct child of GET /orders"
    assert root["attributes"]["load_cache.scenario"] == scenario, root
    gate_to_unlock = int(root["attributes"]["load_cache.gate_to_unlock_ns"])
    if scenario == "overlap":
        assert gate_to_unlock >= 40_000_000, "decoy needs to wake the holder well before its independent unlock"
    else:
        assert gate_to_unlock < 40_000_000, "holder must unlock on receipt of the handoff signal"
    assert cache["self_offcpu_ns"] > 20_000_000, "load_cache should carry the lock wait"
    assert root["offcpu_ns"] >= cache["self_offcpu_ns"], "root must include the child's wait"

    waits = [event for event in cache["events"] if event["comm"] == waiter
             and event["reason"] == "blocked" and any("futex" in frame for frame in event["stack"])]
    assert len(waits) == 1, waits
    chain = waits[0]["chain"]
    assert len(chain) >= 2, chain
    wake, upstream = chain[:2]
    assert wake["comm"] == "lc_holder" and wake["wake_evidence"] == "observed", wake
    assert any("futex" in frame for frame in wake["wake_stack"]), wake
    assert wake["wait_evidence"] == "inferred" and wake["wait_ns"] >= 20_000_000, wake
    assert any("poll" in frame for frame in wake["wait_stack"]), wake
    assert upstream["comm"] == signaler and upstream["wake_evidence"] == "inferred", upstream
    path = [waiter, "lc_holder", signaler]
    causes = [cause for cause in report["causes"] if cause["path"][:3] == path]
    assert causes and all(cause["cause_evidence"] == "inferred_upstream" for cause in causes), causes
    print(f"PASS {scenario}: {' <- '.join(path)}; gate-to-unlock {gate_to_unlock / 1e6:.1f} ms; "
          f"load_cache off-CPU {cache['self_offcpu_ns'] / 1e6:.1f} ms")


if __name__ == "__main__":
    if len(sys.argv) != 2 or sys.argv[1] not in SCENARIOS:
        sys.exit("usage: report.py {handoff|overlap}")
    check(sys.argv[1], json.load(sys.stdin))
