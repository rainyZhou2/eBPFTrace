# Off-CPU overhead

This benchmark measures the cost of the off-CPU source on a real Linux kernel. The workload is a multi-threaded pthread mutex/futex loop; it is not a mock of the eBPF path. The same workload runs with and without the agent, and throughput plus the agent cgroup CPU are recorded.

## Results

Measured on 2026-11-26:

| Item | Value |
|---|---|
| Host | Docker Desktop Linux VM |
| Kernel | `7.0.14-linuxkit`, aarch64 |
| CPUs | 10 vCPU |
| Memory | 8 GiB |
| Workload | 8 threads, 10 s, pthread mutex/futex loop |
| Agent source | off-CPU, `min_block=1us`, `chain_depth=0`, `slow_event=0`, user stacks |
| Policy | disabled |
| Collector | in-memory, no ClickHouse |

`kernel.bpf_stats_enabled` was off for these runs.

| Run | Baseline ops | eTrace ops | Agent CPU |
|---|---:|---:|---:|
| 1 | 343,953,465 | 288,550,861 | 43 ms |
| 2 | 331,787,607 | 290,227,321 | 44 ms |
| 3 | 332,253,236 | 287,331,431 | 52 ms |

Mean baseline: **335,998,103 ops / 10 s** (33.60 M ops/s).
Mean traced: **288,703,204 ops / 10 s** (28.87 M ops/s).
Mean throughput regression: **14.1%**.
Agent cgroup CPU: **0.43–0.52% of one core** while the workload ran.

The agent CPU number excludes BPF program CPU: BPF runs in the context of the traced task, not in the agent process. The throughput delta is therefore the relevant overhead measurement; `kernel.bpf_stats_enabled` can expose the kernel-side run time separately.

## Run

```sh
make build
RUNS=3 THREADS=8 DURATION=10 benchmark/overhead/run.sh
```

The script builds `workload.c` in the builder image, starts an in-memory collector, and runs baseline and traced cases in the same Docker network. It requires a privileged Docker host with BPF support.

## Scope

This is a blocking-heavy microbenchmark, not a database or training benchmark. It isolates the cost of wait capture and stack collection. Results on a different kernel, CPU, or workload will differ; publish the host and workload with any comparison.
