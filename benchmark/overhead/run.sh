#!/usr/bin/env bash
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
ARCH=${HOST_ARCH:-$(uname -m | sed -e 's/x86_64/amd64/' -e 's/aarch64/arm64/')}
BIN=${BIN:-$ROOT/bin/etrace-linux-$ARCH}
BUILDER=${BUILDER:-etrace-cpp-builder}
RUNTIME_IMAGE=${RUNTIME_IMAGE:-debian:trixie}
RUNS=${RUNS:-3}
THREADS=${THREADS:-8}
DURATION=${DURATION:-10}

NET=etrace-overhead
COLLECTOR=etrace-overhead-collector
WORK=etrace-overhead-work
AGENT=etrace-overhead-agent
TMP=$(mktemp -d)
trap 'docker rm -f "$AGENT" "$COLLECTOR" "$WORK" >/dev/null 2>&1 || true; docker network rm "$NET" >/dev/null 2>&1 || true; rm -rf "$TMP"' EXIT

if [ ! -x "$BIN" ]; then
	echo "overhead: missing $BIN; run make build" >&2
	exit 1
fi
cp benchmark/overhead/workload.c "$TMP/workload.c"
docker run --rm -v "$TMP:/src" "$BUILDER" gcc -O2 -pthread /src/workload.c -o /src/workload >/dev/null
docker rm -f "$AGENT" "$COLLECTOR" "$WORK" >/dev/null 2>&1 || true
docker network rm "$NET" >/dev/null 2>&1 || true
docker network create "$NET" >/dev/null

start_collector() {
	docker run -d --name "$COLLECTOR" --network "$NET" \
		-v "$BIN:/usr/local/bin/etrace:ro" \
		-v "$ROOT/benchmark/overhead/collector.yaml:/etc/etrace/collector.yaml:ro" \
		"$RUNTIME_IMAGE" etrace collector -config /etc/etrace/collector.yaml >/dev/null
	sleep 1
}

start_workload() {
	docker run -d --name "$WORK" --network "$NET" --pid=host -v "$TMP:/work" -w /work \
		"$RUNTIME_IMAGE" sh -c "while [ ! -e /work/go ]; do sleep 0.05; done; exec /work/workload $THREADS $DURATION" >/dev/null
}

start_agent() {
	local pid=$1
	docker run -d --name "$AGENT" --network "$NET" --privileged --pid=host -e "TARGET_PID=$pid" \
		-v "$BIN:/usr/local/bin/etrace:ro" \
		-v "$ROOT/benchmark/overhead/agent.yaml:/etc/etrace/agent.yaml:ro" \
		"$RUNTIME_IMAGE" etrace agent -config /etc/etrace/agent.yaml >/dev/null
	sleep 3
}

agent_cpu_usec() {
	docker exec "$AGENT" sh -c 'awk "/^usage_usec/{print \$2}" /sys/fs/cgroup/cpu.stat'
}

run_case() {
	local label=$1 traced=$2 pid before=0 after=0
	rm -f "$TMP/go"
	start_workload
	pid=$(docker inspect -f '{{.State.Pid}}' "$WORK")
	if [ "$traced" = 1 ]; then
		start_agent "$pid"
		before=$(agent_cpu_usec)
	fi
	touch "$TMP/go"
	docker wait "$WORK" >/dev/null
	local ops elapsed
	ops=$(docker logs "$WORK" 2>&1 | awk '/^ops /{print $2}')
	elapsed=$(docker logs "$WORK" 2>&1 | awk '/^ops /{print $4}')
	if [ "$traced" = 1 ]; then
		after=$(agent_cpu_usec)
	fi
	if [ -z "$ops" ]; then
		echo "overhead: workload produced no result" >&2
		docker logs "$WORK" >&2 || true
		exit 1
	fi
	printf '%s,%s,%s,%s\n' "$label" "$ops" "$elapsed" "$(( (after - before) / 1000 ))"
	docker rm -f "$WORK" >/dev/null 2>&1 || true
	if [ "$traced" = 1 ]; then
		docker rm -f "$AGENT" >/dev/null 2>&1 || true
	fi
}

echo "label,ops,elapsed_ns,agent_cpu_ms"
for _ in $(seq 1 "$RUNS"); do
	run_case baseline 0
done
start_collector
for _ in $(seq 1 "$RUNS"); do
	run_case etrace 1
done
