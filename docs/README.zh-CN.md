# eTrace

[English](../README.md) | 简体中文

eTrace 是基于 eBPF 的 CPU 与 GPU 等待时间分析工具。它持续记录线程在哪里等待：无论是因锁、定时器或 I/O 阻塞而离开 CPU，还是卡在 CUDA 的 stream、event 或设备同步上。对每次 off-CPU 等待，它记录是谁结束了它：唤醒者是谁、唤醒者自己又在等什么，最多追溯 4 跳。对每次 GPU 等待，它记录所在的 stream 以及排在前面的 kernel。每次等待都会归属到被阻塞线程当时活跃的 OpenTelemetry span 上，并且 eTrace 可以直接附加到已在运行的进程，因此一个慢请求或一个慢训练 step 可以一路解释到拖住它的那把锁、那次 I/O 或那个 GPU kernel。

## 目录

- [功能](#功能)
- [环境要求](#环境要求)
- [快速开始](#快速开始)
- [工作原理](#工作原理)
- [架构](#架构)
- [部署](#部署)
- [应用接入](#应用接入)
- [查询数据](#查询数据)
- [开发](#开发)

## 功能

- **唤醒因果链。** 每次等待都会记录结束它的唤醒者链路，在内核中直接构建，事后无需关联事件流。
- **GPU 等待。** 在运行中进程的 CUDA 和 NCCL 库上挂 uprobe，记录线程在会阻塞 CPU 的 CUDA 调用里花的时间；无论驱动是自旋还是睡眠都能记录，并带上调用栈、span、stream，以及它排在后面的 kernel 或 NCCL 集合通信。
- **Trace 关联。** off-CPU 和 GPU 等待时间归属到 OpenTelemetry span，并逐级汇总到祖先 span。
- **无帧指针也能拿到完整用户栈。** 在 BPF 中基于 `.eh_frame` 回溯用户栈，发行版默认构建和 `-O2` 构建都能保留完整调用者。
- **单机诊断。** `etrace doctor` 录制单台主机并生成自包含的 HTML 报告，无需部署任何后端。
- **集群部署。** Agent 以 DaemonSet 运行，上报给无状态的 collector，写入 ClickHouse；具备崩溃安全的磁盘缓冲、重试和去重。
- **开放的输出格式。** 火焰图可导出为 pprof 和 collapsed 格式，agent 也可以直接推送到 Grafana Pyroscope。

## 环境要求

- Linux 5.8 及以上，且开启 BTF（`/sys/kernel/btf/vmlinux`）。DWARF 用户栈回溯需要 Linux 5.17 及以上，更老的内核会回退到帧指针回溯。GPU 等待需要 Linux 5.15 及以上（uprobe attach cookie），更老的内核上不记录。
- 具备 CAP_BPF、CAP_PERFMON 和 CAP_SYS_RESOURCE 权限（或 root）。
- glibc 2.38 及以上（Debian 13、Ubuntu 24.04）。libstdc++ 已静态链接。
- 构建机需要 Docker，其余依赖都在构建镜像内。

## 快速开始

### 诊断单台主机

构建二进制（见[构建](#构建)），把 `bin/etrace-linux-<arch>` 复制到目标主机后运行：

```sh
sudo etrace doctor                       # 所有进程，录制 30s，生成 etrace-report.html
sudo etrace doctor -pid 1234 -duration 1m
sudo etrace doctor -comm nginx -slow 50ms -o /tmp/nginx.html
```

文本摘要输出到 stdout，HTML 报告写到 `-o` 指定的路径。报告内容和全部参数见[单机诊断](#单机诊断)。

### 在本地运行完整链路

```sh
make builder   # 只需执行一次
make e2e
```

`make e2e` 用 Docker Compose（`test/e2e/stack`）启动 agent、collector、query 服务、ClickHouse、Jaeger 和一个已接入的 Python 服务。它会等到出现带 trace 的 off-CPU 事件，端到端校验一条 trace，然后销毁整套环境。如果想保留环境自行查看，改为在 `test/e2e/stack` 下执行 `docker compose up -d`；Web UI 地址为 http://127.0.0.1:18080，Jaeger 地址为 http://127.0.0.1:16686。

## 工作原理

### 因果链

当线程 A 被线程 B 唤醒时，eTrace 会检查 B 是否刚结束了一次与 A 的等待有重叠的等待。如果是，就把 B 的链路接到 A 的链路后面。结果读起来像这样：`api` 在 `futex_wait` 中等待 <- 被 `refresher` 在 `unlock` 中唤醒，而 `refresher` 之前在 `do_nanosleep` 中等待 <- 被 `[hardirq]` 在 `hrtimer_wakeup` 中唤醒。

- 每一跳在 `sched_waking` 时于唤醒者上下文中在内核里构建，事后不需要关联任何事件流。
- 来自硬中断、软中断和 idle 的唤醒记为终止跳（`[hardirq]`、`[softirq]`、`[idle]`）。
- 最多记录 4 跳（`chain_depth`）。每一跳带有它的唤醒栈、它自己的等待栈，以及它的等待与原始等待重叠的部分。

### GPU 等待

在 CUDA 驱动的默认调度方式下，等 GPU 的线程在 CPU 上自旋，不会离开 CPU，只看调度事件会漏掉它。eTrace 直接给这些调用计时。

- Agent 在 `/proc/<pid>/maps` 中查找 `libcuda.so`、`libcudart.so` 和 `libnccl.so`（`gpu.library_patterns`，每隔 `gpu.scan_interval` 重新扫描），在会阻塞 CPU 的调用的每个导出变体（`_v2`、`_ptsz`、`_ptds`）上挂一对 uprobe 和 uretprobe：stream、event 和 context 同步，同步拷贝，以及会触发同步的释放和主机内存分配。已在运行的进程无需重启即可覆盖。
- 一次等待是从调用到返回的时间，带上调用时的用户栈和 span，至少持续 `gpu.min_wait` 才记录。cudart 调用 libcuda 时只计最外层的调用。
- 如果线程在调用内部睡眠（`cudaDeviceScheduleBlockingSync`），这段 off-CPU 时间会并入 GPU 等待而不会重复计数，睡眠的唤醒链也挂到这次等待上。来自 NVIDIA 驱动中断处理函数的唤醒显示为 `[gpu]` 跳。
- Launch 跟踪（`gpu.launches`，默认开启）还会探测 kernel 和 graph 的 launch（`cuLaunchKernel`、`cuLaunchKernelEx`、`cudaLaunchKernel`、`cudaLaunchKernelExC`、`cuGraphLaunch`、`cudaGraphLaunch`）、event 记录，以及 `cuModuleGetFunction`/`cuLibraryGetKernel`。它按 stream 和按进程记住最后一次入队的 kernel，并把 event 映射到记录它的 stream。stream 或 event 同步归到对应的 stream；context 级同步、拷贝和默认 stream 上的等待归到进程最后一次 launch。
- 通过 runtime launch 的 kernel 用它的 host stub 符号化得到名字（stub 带着 kernel 的 mangled 名）。通过驱动 launch 的 kernel 用返回其 handle 的模块查找调用命名，所以 agent 附加之前拿到的 handle 显示为 ``[gpu kernel]`[unknown]``。Graph launch 显示为 `CUDA graph`。
- Launch 探针在每次 launch 时都会陷入内核。当一个扫描周期内全机的 launch 速率超过 `gpu.max_launch_rate`（默认 20000/s）时，这些探针会在本次运行中被永久卸载；等待仍然计时，但不再给出 kernel 名。状态导出为 `etrace_offcpu_gpu_launch_tracking`。
- 等待用合成的最内层帧命名：`[gpu]` 加上 CUDA 调用名，然后是 ``[gpu kernel]`<名字>``；如果进程在上一次 GPU 等待之后、这次等待之前发起过 NCCL 集合通信，最内层还会有 ``[gpu]`waiting on ncclAllReduce``，表示线程在等网络和其他 rank。
- 这些等待的 reason 是 `gpu`，和 off-CPU 时间出现在同样的火焰图、原因表、事件和 span 视图中。事件还带有 `gpu_stream` 和 `gpu_launches`（该 stream 上自上一次等待以来入队的 kernel 数）。

### Trace 关联

应用通过 `libetrace_ctx` 按线程发布当前的 trace ID 和 span ID（见[应用接入](#应用接入)）。该库把它们写入一个由序列计数器保护的线程局部记录。Agent 在 `etrace_register_tls` 上挂 uprobe，获知每个线程记录的地址，并在线程阻塞时用 `bpf_probe_read_user` 读取该记录。

- 带 trace 上下文的等待作为独立事件上报；不带的等待做聚合，使样本保持精简。
- Query 服务从 Jaeger（或其他兼容 Jaeger 的 API）拉取 span，把每个事件分配给对应 span，按 span 的时间区间裁剪事件，并把时间汇总到祖先 span。

### 用户栈与符号

- 用户栈在 BPF 中基于每个映射对象的 `.eh_frame` 调用帧信息回溯，所以不带帧指针构建的二进制也能保留完整调用者。`.eh_frame` 在 `strip` 后依然保留。没有回溯表的对象（例如 JIT 代码或 Go 二进制）中的帧使用帧指针回溯。
- Agent 对每个对象按 build ID 只解析一次，并把回溯行加载到共享的 BPF 数组中（`unwind.max_rows`，每行 16 字节）。回溯器遇到未知的进程或地址时，会请求 agent 重新读取 `/proc/<pid>/maps`；同一进程最多每 10s 重新加载一次。`exec` 会在内核中清除该进程的条目。
- `unwind.mode: auto` 在内核支持时（Linux 5.17 及以上）使用该回溯器，否则加载帧指针程序。选择结果会写入日志，并导出为指标 `etrace_offcpu_user_unwinder{mode}`。回溯表用量导出为 `etrace_unwind_*`。
- 符号化优先使用 `.symtab`。对已 strip 的对象，会在进程根目录下的 `/usr/lib/debug/.build-id` 中按 GNU build ID 查找调试文件，然后可选地从 debuginfod 服务器下载（`symbolize.debuginfod.urls`，默认关闭，因为会把 build ID 发送给这些服务器）。下载在后台进行，写入有容量上限的磁盘缓存，并按 build ID 校验。下载完成前，帧只能解析到导出符号。

### 单机诊断

`etrace doctor` 录制单台主机一段时间并输出报告，不需要 collector、ClickHouse 或 query 服务。

- 它首先检查内核版本、BTF 和权限，如果探针无法运行，会说明原因并退出。运行中发现的降级情况（回退到帧指针、BPF map 已满）会随结果一起列出。
- 报告包含：录制结束时仍处于阻塞的线程（疑似卡死，尚未计入总量；仍在 GPU 等待中的线程即使在 CPU 上自旋也会列出，并带上它在等的 kernel）、各进程的 off-CPU 和 GPU 等待时间、沿唤醒链的主要原因、最慢的单次等待，以及交互式火焰图。HTML 文件是自包含的。
- 按 Ctrl-C 可提前结束录制，仍会生成报告。eTrace 自身的线程不计入。
- 录制开始前已经开始的等待不会被看到；在首次采集（每秒一次）前就退出的进程，其帧无法符号化。

| 参数 | 默认值 | 含义 |
|---|---|---|
| `-duration` | `30s` | 录制时长 |
| `-pid` | 全部 | 只录制该进程 |
| `-comm` | 全部 | 只报告该命令名的线程 |
| `-min-block` | `1ms` | 忽略短于该值的等待 |
| `-slow` | `20ms` | 列出不短于该值的单次等待 |
| `-depth` | `4` | 保留的唤醒链跳数（0-4） |
| `-unwind` | `auto` | 用户栈回溯方式：`auto`、`dwarf` 或 `frame_pointer` |
| `-top` | `20` | 每张报告表的行数 |
| `-o` | `etrace-report.html` | HTML 报告路径，`""` 表示不生成 |
| `-pprof` | 无 | 同时以 pprof 格式写出 profile |
| `-debuginfod` | 无 | 逗号分隔的 debuginfod 地址，用于已 strip 的二进制（会向其发送 build ID） |
| `-proc` | `/proc` | procfs 挂载点 |
| `-v` | 关闭 | 输出 agent 内部日志 |

## 架构

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

所有组件由同一个二进制提供：`etrace agent | collector | query | doctor | version`。

- **agent** 运行在每个节点上。它加载 eBPF 数据源，执行处理链（容器元数据、符号化），并把批次发送给 collector。
- **collector** 用 token 认证 agent，检查批次上限，去重后写入存储。
- **query** 提供 HTTP API 和 Web UI，从 ClickHouse 读取数据，从 Jaeger 拉取 span。

| 关注点 | 机制 |
|---|---|
| Collector 故障 | Agent 的 gRPC 客户端通过 headless Service 解析所有副本，轮询负载均衡，并跳过健康检查失败的副本。存储不可达时 collector 会把自己标记为 NOT_SERVING。 |
| 投递保证 | 批次在发送前先写入崩溃安全的磁盘缓冲（spool），collector 接收后才确认。重试使用带抖动的指数退避。 |
| 重试导致的重复 | 每个批次有唯一 ID。Collector 维护最近 ID 的 LRU，ClickHouse 通过 `insert_deduplication_token` 在副本间去重。 |
| 背压 | Spool 有字节预算，超出时优先淘汰最旧的批次。BPF map 溢出计入 `etrace_offcpu_bpf_drops`。 |
| 扩展 | Collector 和 query 服务无状态，可水平扩展（HPA、PDB、跨可用区分布）。存储通过 ClickHouse 分片和副本扩展。 |
| 内核开销 | 在 `sched_switch`/`sched_waking` 上以 `tp_btf` 方式挂载，在内核中聚合，使用按周期翻转的双缓冲 map，并用 LRU map 保存每个任务的状态。DWARF 回溯每帧需要几次二分查找和用户内存读取，开销高于帧指针回溯；设置 `unwind.mode: frame_pointer` 可关闭它。 |

## 部署

### Kubernetes

`deploy/k8s` 包含命名空间与 Secret、agent DaemonSet，以及 collector 和 query 的 Deployment 及其 Service、PDB、HPA。清单假定集群中已有 ClickHouse（`clickhouse.etrace.svc:9000`）和 Jaeger query（`http://jaeger-query.observability.svc:16686`），请按实际环境修改 `env` 配置。

```sh
make image                                   # 多架构运行镜像 etrace:<version>
# 编辑 deploy/k8s/namespace.yaml：替换 etrace-secrets 中的 change-me
kubectl apply -f deploy/k8s/namespace.yaml
kubectl apply -f deploy/k8s/
```

Collector 启动时会创建 ClickHouse 表（`auto_migrate: true`），query 服务不会。把 `CLICKHOUSE_CLUSTER` 设为集群名即可创建复制表和分布式表。

### 配置

每个服务默认读取 `/etc/etrace/<command>.yaml`，可用 `-config path` 覆盖。配置中的 `${VAR}` 会用环境变量展开，因此密钥不必写进配置文件。参考配置位于 `configs/`：

| 文件 | 用途 |
|---|---|
| `configs/agent.yaml` | Agent：数据源、处理器、导出器、spool、重试 |
| `configs/collector.yaml` | Collector：token、去重、批次上限、ClickHouse 存储 |
| `configs/query.yaml` | Query 服务：HTTP 监听、查询上限、存储、trace 后端 |
| `configs/standalone-collector.yaml` | 使用内存存储的 collector，用于本地测试 |

### 运维

每个服务都有一个管理 HTTP 端口（agent `:9431`、collector `:9432`、query `:9433`），提供 `/metrics`（Prometheus）、`/healthz` 和 `/readyz`。

## 应用接入

Trace 关联需要应用发布当前的 span。不接入时，eTrace 依然会记录全部 off-CPU 时间、唤醒链和调用栈，只是这些等待不会关联到 span。

### C 及其他原生语言

在 `sdk/c` 中构建并安装 `libetrace_ctx`：

```sh
make -C sdk/c && sudo make -C sdk/c install   # 默认安装到 /usr/local
```

链接该库，并在某个 span 成为线程当前 span 或不再是当前 span 时，调用 `etrace_ctx.h` 中的函数：

```c
void etrace_set_context(const uint8_t trace_id[16], const uint8_t span_id[8]);
void etrace_clear_context(void);
```

两者可在任意线程调用，且不会阻塞。Agent 通过 `trace_context.library_patterns` 找到该库，并挂载到 `etrace_register_tls`；应用不应直接调用这个函数。

### Python

`sdk/python`（`etrace-otel`，Python 3.9+）提供一个 OpenTelemetry runtime context，在每次 attach 和 detach 时发布当前 span。它通过动态链接器或 `ETRACE_CTX_LIBRARY` 环境变量加载 `libetrace_ctx`。

```sh
pip install ./sdk/python
export OTEL_PYTHON_CONTEXT=etrace_contextvars_context
```

也可以在创建任何 span 之前调用 `etrace_otel.install()`。

## 查询数据

### Web UI

Query 服务在其 HTTP 端口的 `/` 提供浏览器界面：筛选条件（时间范围、node、namespace、pod、comm、pid、跳数）、支持搜索和 pprof/collapsed 下载的交互式火焰图、原因表、带调用栈和唤醒链的事件列表，以及按 span 展示的 trace 视图。它是嵌入在二进制中的静态页面，只调用下列 API，并以严格的 Content-Security-Policy 提供服务。

### HTTP API

```
GET /api/v1/offcpu/flamegraph?from=&to=&node=&comm=&pid=&container_id=&namespace=&pod=&label=k=v&depth=0..4&format=json|collapsed|pprof
GET /api/v1/offcpu/causes?from=&to=&comm=&depth=&limit=
GET /api/v1/offcpu/events?from=&to=&trace_id=&span_id=&min_duration=50ms&limit=
GET /api/v1/traces/{trace_id}/offcpu
```

- 所有接口都接受相同的筛选参数。`namespace` 和 `pod` 来自 agent 的 `container` 处理器：pod UID 从任务的 cgroup 读取，pod 名、命名空间和容器名从宿主机上容器运行时的状态读取（containerd 和 CRI-O 的 OCI 配置、Docker labels），经由 `/proc/1/root` 访问。不需要访问 Kubernetes API，因此拿不到 pod labels。
- `flamegraph`：每一跳以 `--` 帧为分隔叠在上方，依次是 `<- comm`、它的唤醒栈、`waiting: comm` 和它的等待栈。`format=collapsed` 的输出可用于 `flamegraph.pl` 和 speedscope。`format=pprof` 以 gzip 压缩的 pprof profile 返回相同路径（样本类型为 `off_cpu/nanoseconds` 和 `waits/count`），可用于 `go tool pprof`、speedscope 或 Pyroscope；`etrace doctor -pprof file` 也会写出同样格式的文件。
- `causes`：按任务路径分组的阻塞时间，路径终止于能够解释该延迟的最深一跳。
- `traces/{id}/offcpu`：每个 span 的 `offcpu_ns`（含子孙 span）和 `self_offcpu_ns`、背后的事件以及原因。未配置 trace 后端时，事件不会被归属，并由 `partial` 字段说明原因。

### 导出到 Pyroscope

设置 `exporter.driver: pyroscope`（`url`、`service_name`，可选 `tenant_id` 和 basic auth）后，agent 会推送到 Grafana Pyroscope 而不是 eTrace collector。每个批次推送一次，每个任务身份一条序列，标签为 `node`、`comm`、`namespace`、`pod`、`container` 以及 agent 的静态标签，profile 类型为 `offcpu:off_cpu:nanoseconds`。GPU 等待作为单独的序列推送，profile 类型为 `gpu:gpu_wait:nanoseconds`，不会累加进 off-CPU profile。推送语义为至少一次：响应丢失后的重试会把同一时间窗口存两次。由于 OTLP profiles 信号仍在开发中，目前不导出该格式。

## 开发

### 构建

eTrace 使用 C++20，以 CMake 构建，依赖通过 vcpkg 锁定版本（`vcpkg.json`）。所有构建都在 Linux 构建镜像中进行，宿主机只需要 Docker：

```sh
make builder         # 构建 etrace-cpp-builder：工具链、用于 BPF 的 clang、vcpkg 依赖
make build           # 编译 BPF 对象和 bin/etrace-linux-<arch>
make image           # 运行镜像（debian:trixie-slim）
make fmt             # 用 clang-format 格式化 src 和 test（fmt-check 只检查）
make bpf-ide-headers # 把 Linux/libbpf 头文件复制到 build/bpf-include，供 clangd 使用（.clangd）
```

- BPF 程序按构建机的架构编译并嵌入二进制，所以 amd64 二进制要在 amd64 构建镜像中构建。
- 源站不可达时，`build/vcpkg-fetch.sh` 会把 GitHub 和 sourceware 的下载转到镜像源（构建参数 `GITHUB_PROXY`、`SOURCEWARE_MIRROR`）。
- 传入 `CMAKE_FLAGS=-DETRACE_SANITIZE=address,undefined`（或 `thread`）可开启 sanitizer 构建。

### 测试

```sh
make test            # 单元测试（GoogleTest）
make test-clickhouse # 针对临时 ClickHouse 实例测试 ClickHouse 驱动
make test-kernel     # 在特权容器中加载 BPF 程序，校验唤醒链、trace 归属和 DWARF 用户栈
make e2e             # agent + collector + query + ClickHouse + Jaeger + 已接入的 Python 服务
```

### 目录结构

```
api/proto            通信协议（protobuf、gRPC）
src/main.cpp         单一二进制入口：agent | collector | query | doctor
src/model            各组件共享的领域类型
src/wire             protobuf 与 model 之间的转换
src/agent            处理流水线、插件契约（plugin.h）和驱动工厂
  source/offcpu      eBPF 程序、libbpf 加载器、uprobe 管理（trace 上下文、GPU 等待）、
                     回溯表管理（unwinder）
  processor/*        容器 ID、符号化
  exporter/*         collector 客户端、Pyroscope
src/collector        接入服务和 gRPC handler
src/query            查询服务、火焰图与 pprof 渲染、HTTP API、Web UI
src/storage          存储契约、memory 与 clickhouse 驱动
src/symbol           ELF 读取、kallsyms、/proc/pid/maps、符号表、build ID、debuginfod
src/unwind           .eh_frame 解析器，产出按地址排序的回溯行
src/tracing          trace 后端契约、Jaeger 驱动
src/doctor           单机诊断：预检、录制会话、报告
src/app              各服务的配置与组装
src/platform         不含领域知识的通用基础设施（配置、日志、生命周期、
                     spool、gRPC/HTTP 服务、指标、TLS）
sdk/c                libetrace_ctx：按线程发布 trace 上下文
sdk/python           Python 的 OpenTelemetry 集成
test/unit            单元测试和进程内集成测试（GoogleTest）
test/kernel          需要 root、BTF 和宿主机 PID 命名空间的 BPF 测试
test/scenario        内核测试用的已知唤醒链负载
test/e2e/stack       docker compose 端到端环境
deploy/k8s           DaemonSet、Deployment、Service、PDB、HPA
configs              参考配置
docs                 文档（中文 README）
build                构建与运行 Dockerfile、vcpkg overlay
```

`src` 下每个目录是一个 CMake 库，依赖方向向内：`platform` 不依赖 eTrace 的任何其他部分，业务代码只依赖接口（`storage::Writer`、`storage::Reader`、`agent::Source`、`agent::Exporter`、`tracing::Backend`）。具体驱动只在工厂函数和 `src/app` 中选择。面向贡献者和编码 Agent 的工程规范见 [C++AGENTS.md](../C++AGENTS.md)。

### 扩展

数据源、处理器、导出器、存储驱动和 trace 后端都通过配置中的 `driver:` 选择。新增一种时，实现对应接口并在其工厂中加一个分支：`src/agent/plugins.cpp` 中的 `agent::new_source`、`new_processor` 和 `new_exporter`，`src/storage/drivers.cpp` 中的 `storage::open`，或 `src/tracing/tracing.cpp` 中的 `tracing::open`。驱动选项是用 `decode_fields` 解码的普通结构体，未知字段会被拒绝。
