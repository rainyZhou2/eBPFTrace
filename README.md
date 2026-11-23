# eTrace

English | [简体中文](docs/README.zh-CN.md)

eTrace is an eBPF profiler for wait time on the CPU and the GPU. It continuously records where threads wait, whether blocked off-CPU on a lock, timer, or I/O, or held up in CUDA stream, event, or device synchronization. For each off-CPU wait it records who ended it: the waker, what the waker was itself waiting on, and so on for up to 4 hops. For each GPU wait it records the stream and the kernel queued ahead of it. Every wait is attributed to the OpenTelemetry span that was active on the blocked thread, and eTrace attaches to processes that are already running, so a slow request or training step can be explained down to the lock, I/O, or GPU kernel that held it up.

## Contents

- [Features](#features)
- [Requirements](#requirements)
- [Quick start](#quick-start)
- [How it works](#how-it-works)
- [Architecture](#architecture)
- [Deployment](#deployment)
- [Instrumenting applications](#instrumenting-applications)
- [Querying data](#querying-data)
- [Development](#development)

## Features

- **Wakeup causal chains.** Every wait records the chain of wakers that ended it, built in kernel with no event joining afterwards.
- **GPU waits.** Time threads spend in host-blocking CUDA calls is recorded from uprobes on the CUDA and NCCL libraries of running processes, whether the driver spins or sleeps, with the call stack, the span, the stream, and the kernel or NCCL collective it is queued behind.
- **Trace correlation.** Off-CPU and GPU wait time is attributed to OpenTelemetry spans and rolled up to their ancestors.
- **Full user stacks without frame pointers.** User stacks are walked from `.eh_frame` in BPF, so default distribution and `-O2` builds keep all their callers.
- **Single-host diagnosis.** `etrace doctor` records one host and writes a self-contained HTML report, with no backend to install.
- **Cluster deployment.** An agent DaemonSet ships to stateless collectors that write to ClickHouse, with a crash-safe spool, retries, and deduplication.
- **Open output formats.** Flame graphs export as pprof and collapsed stacks, and the agent can push to Grafana Pyroscope.

## Requirements

- Linux 5.8 or later with BTF (`/sys/kernel/btf/vmlinux`). The DWARF user stack walker needs Linux 5.17 or later; older kernels fall back to frame pointers. GPU waits need Linux 5.15 or later (uprobe attach cookies); on older kernels they are not recorded.
- Capabilities CAP_BPF, CAP_PERFMON, and CAP_SYS_RESOURCE (or root).
- glibc 2.38 or later (Debian 13, Ubuntu 24.04). libstdc++ is linked statically.
- gRPC 1.80 or later. The builder image pins 1.81; distribution packages such as Debian 13's 1.51 are too old for the TLS credentials code in `src/platform`.
- Docker on the build host. Everything else is built inside the builder image.

## Quick start

### Diagnose one host

Build the binary (see [Build](#build)), copy `bin/etrace-linux-<arch>` to the target host, and run:

```sh
sudo etrace doctor                       # all processes, 30s, writes etrace-report.html
sudo etrace doctor -pid 1234 -duration 1m
sudo etrace doctor -comm nginx -slow 50ms -o /tmp/nginx.html
```

The text summary goes to stdout and the HTML report is written to `-o`. See [Single-host diagnosis](#single-host-diagnosis) for what the report contains and for all flags.

### Run the full stack locally

```sh
make builder   # once
make e2e
```

`make e2e` starts the agent, collector, query service, ClickHouse, Jaeger, and an instrumented Python service with Docker Compose (`test/e2e/stack`). It waits for traced off-CPU events, checks one trace end to end, and tears the stack down. To keep it running for exploration, run `docker compose up -d` in `test/e2e/stack` instead; the web UI is then at http://127.0.0.1:18080 and Jaeger at http://127.0.0.1:16686.

## How it works

### Causal chains

When thread A is woken by B, eTrace checks whether B had just finished a wait of its own that overlapped A's wait. If it had, B's chain is appended to A's. The result reads, for example: `api` waited in `futex_wait` <- woken by `refresher` in `unlock`, which had been waiting in `do_nanosleep` <- woken by `[hardirq]` in `hrtimer_wakeup`.

- Hops are built in kernel at `sched_waking`, in the waker's context, so no event stream has to be joined later.
- Wakeups from hard IRQ, soft IRQ, and idle are recorded as terminal hops (`[hardirq]`, `[softirq]`, `[idle]`).
- Up to 4 hops are recorded (`chain_depth`). Each hop carries its wake stack, its own wait stack, and the part of its wait that overlapped the original wait.

### GPU waits

With the CUDA driver's default scheduling, a thread waiting for the GPU spins on the CPU and never goes off-CPU, so scheduler events alone miss it. eTrace times the calls themselves.

- The agent finds `libcuda.so`, `libcudart.so`, and `libnccl.so` in `/proc/<pid>/maps` (`gpu.library_patterns`, rescanned every `gpu.scan_interval`) and attaches a uprobe and uretprobe pair to every exported variant (`_v2`, `_ptsz`, `_ptds`) of the calls that block the host: stream, event, and context synchronization, synchronous copies, and the frees and host allocations that synchronize. Running processes are covered without a restart.
- A wait is the time from call to return, recorded with the user stack and span at the call, when it lasts at least `gpu.min_wait`. When cudart calls into libcuda, only the outer call counts.
- If the thread sleeps inside the call (`cudaDeviceScheduleBlockingSync`), that off-CPU time is folded into the GPU wait instead of being counted twice, and the wakeup chain of the sleep is attached to it. A wakeup from the NVIDIA driver's interrupt handler shows as a `[gpu]` hop.
- Launch tracking (`gpu.launches`, on by default) also probes kernel and graph launches (`cuLaunchKernel`, `cuLaunchKernelEx`, `cudaLaunchKernel`, `cudaLaunchKernelExC`, `cuGraphLaunch`, `cudaGraphLaunch`), event records, and `cuModuleGetFunction`/`cuLibraryGetKernel`. A runtime launch's internal driver call is not counted twice, including `cudaGraphLaunch` into `cuGraphLaunch`. It keeps the last kernel queued per stream and per process, and maps events to the stream they were recorded on. A stream or event sync is attributed to its stream; context-wide syncs, copies, and waits on the default stream to the process's last launch.
- Kernels launched through the runtime are named by symbolizing their host stub, which carries the kernel's (mangled) name. Kernels launched through the driver are named from the module lookup that returned their handle, so handles obtained before the agent attached show as ``[gpu kernel]`[unknown]``. Graph launches show as `CUDA graph`.
- Launch probes trap on every launch. When the host-wide launch rate over a scan interval exceeds `gpu.max_launch_rate` (20000/s by default), they are detached for the rest of the run; waits are still timed but name no kernel. The state is exported as `etrace_offcpu_gpu_launch_tracking`.
- The wait is named by synthetic innermost frames: `[gpu]` and the CUDA call, then ``[gpu kernel]`<name>``, then ``[gpu]`waiting on ncclAllReduce`` when the process issued an NCCL collective after its previous GPU wait and before this one, which means the thread waits for the network and the other ranks.
- These waits have reason `gpu` and appear in the same flame graphs, causes, events, and span views as off-CPU time. Events also carry `gpu_stream` and `gpu_launches`, the kernels queued on that stream since its previous wait.

### Trace correlation

Applications publish the active trace and span IDs per thread through `libetrace_ctx` (see [Instrumenting applications](#instrumenting-applications)). The library writes them to a thread-local record guarded by a sequence counter. The agent attaches a uprobe to `etrace_register_tls`, learns each thread's record address, and reads the record with `bpf_probe_read_user` when the thread blocks.

- Waits with a trace context are emitted as individual events. Waits without one are aggregated, which keeps samples small.
- The query service fetches spans from Jaeger (or another Jaeger-compatible API). It assigns each event to its span, clips the event to the span's interval, and rolls the time up to ancestor spans.

### User stacks and symbols

- User stacks are walked in BPF from the `.eh_frame` call frame information of each mapped object, so binaries built without frame pointers keep all their callers. `.eh_frame` survives `strip`. Frames in objects without a table, such as JIT code or Go binaries, are walked with frame pointers.
- The agent parses each object once per build ID and loads the rows into a shared BPF array (`unwind.max_rows`, 16 bytes per row). When the walker meets a process or address it does not know, it asks the agent to read `/proc/<pid>/maps` again; one process is reloaded at most every 10s. `exec` drops the process's entry in kernel.
- `unwind.mode: auto` uses this walker when the kernel accepts it (Linux 5.17 or later) and otherwise loads the frame pointer program. The choice is logged and exported as `etrace_offcpu_user_unwinder{mode}`. Table usage is exported as `etrace_unwind_*`.
- Symbolization prefers `.symtab`. For stripped objects it looks up the debug file by GNU build ID under `/usr/lib/debug/.build-id` in the process root, then optionally downloads it from debuginfod servers (`symbolize.debuginfod.urls`, off by default because it sends build IDs to those servers). Downloads run in the background into a bounded disk cache and are verified against the build ID. Until one arrives, frames resolve to exported symbols only.

### Single-host diagnosis

`etrace doctor` records one host for a while and prints a report, with no collector, ClickHouse, or query service.

- It first checks the kernel version, BTF, and privileges, and stops with the reason if the probes cannot run. Degradations found during the run (frame pointer fallback, full BPF maps) are listed with the results.
- The report has the threads still blocked when recording stopped (hang candidates, not yet in the totals; a thread still inside a GPU wait is listed with the kernel it waits for, even while it spins on the CPU), off-CPU and GPU wait time per process, the top causes along wakeup chains, the slowest single waits, and an interactive flame graph. The HTML file is self-contained.
- Recording stops early on Ctrl-C and still produces the report. eTrace's own threads are left out.
- Waits that began before recording started are not seen, and frames of a process that exits before its first collection (every second) stay unsymbolized.

| Flag | Default | Meaning |
|---|---|---|
| `-duration` | `30s` | How long to record |
| `-pid` | all | Record only this process |
| `-comm` | all | Report only threads with this command name |
| `-min-block` | `1ms` | Ignore waits shorter than this |
| `-slow` | `20ms` | List single waits at least this long |
| `-depth` | `4` | Wakeup chain hops to keep (0-4) |
| `-unwind` | `auto` | User stack unwinder: `auto`, `dwarf`, or `frame_pointer` |
| `-top` | `20` | Rows per report table |
| `-o` | `etrace-report.html` | HTML report path, `""` to skip |
| `-pprof` | none | Also write the profile in pprof format |
| `-debuginfod` | none | Comma-separated debuginfod URLs for stripped binaries (sends build IDs to them) |
| `-proc` | `/proc` | procfs mount |
| `-v` | off | Log agent internals |

## Architecture

```
 node                          stateless, N replicas              storage
┌──────────────────────┐  gRPC  ┌──────────────────┐        ┌────────────────────┐
│ etrace agent         │──────▶ │ etrace collector │──────▶ │ ClickHouse          │
│  source  (eBPF)      │ RR+    │  auth / limits    │ dedup  │ Replicated + Dist.  │
│  processor chain     │ health │  idempotent write │ token  └─────────▲──────────┘
│  disk spool + retry  │        └──────────────────┘                  │
└──────────────────────┘                     ┌──────────────────┐     │
                                             │ etrace query     │─────┘
                                             │ flamegraph / wake │
                                             └──────────────────┘
```

One binary provides every component: `etrace agent | collector | query | doctor | version`.

- **agent** runs on every node. It loads the eBPF source, runs the processor chain (container metadata, symbolization), and ships batches to the collector.
- **collector** authenticates agents with a token, enforces batch limits, deduplicates, and writes to storage.
- **query** serves the HTTP API and web UI, reading from ClickHouse and fetching spans from Jaeger.

| Concern | Mechanism |
|---|---|
| Collector failure | The agent's gRPC client resolves every replica through a headless Service, balances round robin, and skips replicas whose health check fails. Collectors mark themselves NOT_SERVING when storage is unreachable. |
| Delivery guarantee | Batches are written to a crash-safe disk spool before shipping and acknowledged only after the collector accepts them. Retries use exponential backoff with jitter. |
| Duplicates from retries | Each batch has a unique ID. The collector keeps an LRU of recent IDs, and ClickHouse deduplicates across replicas with `insert_deduplication_token`. |
| Backpressure | The spool has a byte budget and evicts the oldest batches first. BPF map overflows are counted in `etrace_offcpu_bpf_drops`. |
| Scaling | The collector and query services hold no state, so they scale horizontally (HPA, PDB, zone spreading). Storage scales with ClickHouse shards and replicas. |
| Kernel overhead | `tp_btf` attach on `sched_switch`/`sched_waking`, aggregation in kernel, double-buffered maps flipped on each interval, and LRU maps for per-task state. The DWARF walker costs a few binary searches and user memory reads per frame, more than frame pointer walking; `unwind.mode: frame_pointer` switches it off. |

## Deployment

### Kubernetes

`deploy/k8s` contains the namespace and secret, the agent DaemonSet, and the collector and query Deployments with their Services, PDB, and HPA. The manifests expect an existing ClickHouse at `clickhouse.etrace.svc:9000` and Jaeger query at `http://jaeger-query.observability.svc:16686`; change the `env` entries to match your cluster.

```sh
make image                                   # multi-arch runtime image etrace:<version>
# edit deploy/k8s/namespace.yaml: replace the change-me values in etrace-secrets
kubectl apply -f deploy/k8s/namespace.yaml
kubectl apply -f deploy/k8s/
```

The collector creates the ClickHouse tables on start (`auto_migrate: true`); the query service does not. Set `CLICKHOUSE_CLUSTER` to a cluster name to create replicated and distributed tables.

### Configuration

Each service reads `/etc/etrace/<command>.yaml` by default; pass `-config path` to override. `${VAR}` references are expanded from the environment, so secrets stay out of config files. Reference configs are in `configs/`:

| File | Purpose |
|---|---|
| `configs/agent.yaml` | Agent: sources, processors, exporter, spool, retry |
| `configs/collector.yaml` | Collector: tokens, dedup, batch limits, ClickHouse storage |
| `configs/query.yaml` | Query service: HTTP listener, limits, storage, trace backend |
| `configs/standalone-collector.yaml` | Collector with in-memory storage for local testing |

### Operations

Each service exposes an admin HTTP listener (agent `:9431`, collector `:9432`, query `:9433`) with `/metrics` (Prometheus), `/healthz`, and `/readyz`.

## Instrumenting applications

Trace correlation needs the application to publish its current span. Without it, eTrace still records all off-CPU time, chains, and stacks; the waits are just not tied to spans.

### C and other native languages

Build and install `libetrace_ctx` from `sdk/c`:

```sh
make -C sdk/c && sudo make -C sdk/c install   # installs to /usr/local by default
```

Link against it and call these functions from `etrace_ctx.h` whenever a span becomes current or stops being current on a thread:

```c
void etrace_set_context(const uint8_t trace_id[16], const uint8_t span_id[8]);
void etrace_clear_context(void);
```

Both are safe to call from any thread and never block. The agent finds the library through `trace_context.library_patterns` and attaches to `etrace_register_tls`, which applications should not call.

### Python

`sdk/python` (`etrace-otel`, Python 3.9+) installs an OpenTelemetry runtime context that publishes the current span on every attach and detach. It loads `libetrace_ctx` through the dynamic linker or from `ETRACE_CTX_LIBRARY`.

```sh
pip install ./sdk/python
export OTEL_PYTHON_CONTEXT=etrace_contextvars_context
```

Alternatively, call `etrace_otel.install()` before any span is started.

## Querying data

### Web UI

The query service serves a browser UI at `/` on its HTTP port: filters (time range, node, namespace, pod, comm, pid, hops), an interactive flame graph with search and pprof/collapsed download, the causes table, events with their stacks and wakeup chains, and the per-span view of a trace. It is a static page embedded in the binary that only calls the API below, served with a strict Content-Security-Policy.

### HTTP API

```
GET /api/v1/offcpu/flamegraph?from=&to=&node=&comm=&pid=&container_id=&namespace=&pod=&label=k=v&depth=0..4&format=json|collapsed|pprof
GET /api/v1/offcpu/causes?from=&to=&comm=&depth=&limit=
GET /api/v1/offcpu/events?from=&to=&trace_id=&span_id=&min_duration=50ms&limit=
GET /api/v1/traces/{trace_id}/offcpu
```

- Every endpoint accepts the same filters. `namespace` and `pod` come from the agent's `container` processor: the pod UID is read from the task's cgroup, and pod name, namespace, and container name from the container runtime's state on the host (containerd and CRI-O OCI configs, Docker labels), reached through `/proc/1/root`. No Kubernetes API access is needed, so pod labels are not available.
- `flamegraph`: each hop is stacked above a `--` frame as `<- comm`, then its wake stack, then `waiting: comm` and its wait stack. `format=collapsed` output works with `flamegraph.pl` and speedscope. `format=pprof` returns the same paths as a gzip pprof profile (sample types `off_cpu/nanoseconds` and `waits/count`) for `go tool pprof`, speedscope, or Pyroscope; `etrace doctor -pprof file` writes one too.
- `causes`: blocking time grouped by task path, ending at the deepest hop whose wait explains the delay.
- `traces/{id}/offcpu`: per-span `offcpu_ns` (including descendants) and `self_offcpu_ns`, the events behind them, and the causes. Without a trace backend, events come back unattributed and `partial` explains why.

### Exporting to Pyroscope

The agent can push to Grafana Pyroscope instead of the eTrace collector with `exporter.driver: pyroscope` (`url`, `service_name`, optional `tenant_id` and basic auth). Each batch is one push with one series per task identity, labelled `node`, `comm`, `namespace`, `pod`, `container` and the agent's static labels, under the profile type `offcpu:off_cpu:nanoseconds`. GPU waits are pushed as separate series under `gpu:gpu_wait:nanoseconds`, so they do not add to the off-CPU profile. Pushes are at least once: a retry after a lost response stores the window twice. OTLP profiles are not exported because the signal is still in development.

## Development

### Build

eTrace is C++20, built with CMake and dependencies pinned through vcpkg (`vcpkg.json`). Everything builds inside a Linux builder image, so the host only needs Docker:

```sh
make builder         # builds etrace-cpp-builder: toolchain, clang for BPF, vcpkg dependencies
make build           # compiles BPF objects and bin/etrace-linux-<arch>
make image           # runtime image (debian:trixie-slim)
make fmt             # clang-format src and test (fmt-check to verify only)
make bpf-ide-headers # copies Linux/libbpf headers to build/bpf-include for clangd (.clangd)
```

- The BPF programs are compiled for the build host's architecture and embedded in the binary, so an amd64 binary is built on an amd64 builder.
- The builder image pulls `debian:trixie`, `deb.debian.org` and `github.com` by default. `BASE_IMAGE`, `APT_MIRROR`, `GITHUB_PROXY` and `SOURCEWARE_MIRROR` build args route the same steps through mirrors, for example `make builder BUILDER_BUILD_ARGS='--build-arg GITHUB_PROXY=https://gh-proxy.com'`; `build/vcpkg-fetch.sh` applies the last two to vcpkg downloads as well.
- Pass `CMAKE_FLAGS=-DETRACE_SANITIZE=address,undefined` (or `thread`) to build with sanitizers.

### Test

```sh
make test            # unit tests (GoogleTest)
make test-clickhouse # ClickHouse driver test against a throwaway server
make test-kernel     # loads the BPF programs in a privileged container and checks chains, trace attribution and DWARF user stacks
make e2e             # agent + collector + query + ClickHouse + Jaeger + an instrumented Python service
```

The kernel tests can also run in QEMU, without giving the test process access to the host kernel:

```sh
make kernel-bin scenarios
test/kernel/qemu/run.sh --kernel-image ubuntu:22.04 --expect 5.15
```

`run.sh` boots a distribution kernel with an initramfs that contains the test binary, the scenario binaries and a static busybox, runs `etrace_kernel_tests` in the guest, and fails on skipped tests. On a 5.15 kernel the DWARF-only tests are excluded because `bpf_loop` is unavailable; the frame-pointer path is still tested.

`.github/workflows/ci.yml` runs the unit tests on amd64 and arm64, and the kernel tests in QEMU on Ubuntu 5.15 and 6.8 kernels. The QEMU harness uses two vCPUs and KVM when the runner exposes `/dev/kvm`; hosted runners fall back to TCG. `.github/workflows/release.yml` turns a `v*` tag into checksummed binary artifacts and a GitHub release: `git tag -a v0.1.0 -m v0.1.0 && git push origin v0.1.0`. Container images are still built locally with `make image`.

### Layout

```
api/proto            wire contract (protobuf, gRPC)
src/main.cpp         single binary: agent | collector | query | doctor
src/model            domain types shared by all components
src/wire             protobuf <-> model conversion
src/agent            pipeline, plus plugin contracts (plugin.h) and driver factories
  source/offcpu      eBPF program, libbpf loader, uprobe manager (trace context, GPU waits),
                     unwind table manager (unwinder)
  processor/*        container id, symbolization
  exporter/*         collector client, Pyroscope
src/collector        ingest service and gRPC handler
src/query            query service, flamegraph and pprof rendering, HTTP API, web UI
src/storage          storage contract, memory and clickhouse drivers
src/symbol           ELF reader, kallsyms, /proc/pid/maps, symbol tables, build IDs, debuginfod
src/unwind           .eh_frame parser producing address-sorted unwind rows
src/tracing          trace backend contract, Jaeger driver
src/doctor           single-host diagnosis: preflight checks, recording session, report
src/app              per-service config and wiring
src/platform         reusable infrastructure with no domain knowledge (config, logging,
                     lifecycle, spool, gRPC/HTTP servers, metrics, TLS)
sdk/c                libetrace_ctx: per-thread trace context publisher
sdk/python           OpenTelemetry integration for Python
test/unit            unit and in-process integration tests (GoogleTest)
test/kernel          BPF tests that need root, BTF and the host PID namespace
                     qemu/ runs them in a throwaway VM
test/scenario        workloads with known chains for kernel tests
test/e2e/stack       docker compose end-to-end stack
deploy/k8s           DaemonSet, Deployments, Services, PDB, HPA
configs              reference configs
docs                 documentation (Chinese README)
build                builder and runtime Dockerfiles, vcpkg overlays
```

Each directory under `src` is one CMake library and dependencies point inward: `platform` depends on nothing in eTrace, and business code depends on interfaces (`storage::Writer`, `storage::Reader`, `agent::Source`, `agent::Exporter`, `tracing::Backend`). Concrete drivers are chosen only in the factory functions and `src/app`.

### Extending

Sources, processors, exporters, storage drivers, and trace backends are selected by `driver:` in config. To add one, implement the interface and add a branch to its factory: `agent::new_source`, `new_processor` and `new_exporter` in `src/agent/plugins.cpp`, `storage::open` in `src/storage/drivers.cpp`, or `tracing::open` in `src/tracing/tracing.cpp`. Driver options are plain structs decoded with `decode_fields`; unknown keys are rejected.

## License

Apache-2.0; see [LICENSE](LICENSE). The eBPF program in `src/agent/source/offcpu/bpf/offcpu.bpf.c` and the scenario programs in `test/scenario` keep the kernel-community dual license `GPL-2.0 OR BSD-2-Clause` declared in their headers.
