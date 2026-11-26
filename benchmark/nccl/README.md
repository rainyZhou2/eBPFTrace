# NCCL attribution accuracy

Status: **not measured on this host**. The benchmark host has no NVIDIA GPU or driver (`nvidia-smi` is absent and `/dev/nvidia*` does not exist), so a real CUDA/NCCL training run cannot execute here. No accuracy number is published from a simulated NCCL path.

## Required environment

- Two or more NVIDIA GPUs visible to the container or host
- CUDA driver and a PyTorch build with NCCL
- eTrace agent built for the same host, with GPU probes enabled
- A query service or ClickHouse collector to read the GPU events

## Method

1. Run a fixed-step PyTorch DDP training job with a known collective in each step, for example `all_reduce` after the backward pass. Record a ground-truth JSONL entry for every collective: rank, iteration, collective name, start and end timestamps.
2. Run the same job with `NCCL_DEBUG=INFO` and `NCCL_DEBUG_SUBSYS=COLL` so NCCL's own log independently labels the collective.
3. Query `/api/v1/offcpu/events` for the agent's GPU events in the same window. An attributed event contains a synthetic `[gpu]` frame named `waiting on ncclAllReduce` (or the other `nccl*` collective) and the CUDA call frame.
4. Match each ground-truth collective to an eTrace event by rank, time window, and collective name. Report precision, recall, and F1 per collective and overall. A match requires the correct collective name; a GPU wait with no collective is a true negative only when no collective was issued in the window.

## Names

Use these names consistently in results and code:

| Concept | Name |
|---|---|
| workload without profiling | `baseline` |
| workload with eTrace | `etrace` |
| collective operations | `ncclAllReduce`, `ncclBroadcast`, `ncclReduce`, `ncclAllGather`, `ncclReduceScatter`, `ncclSend`, `ncclRecv` |
| metric | precision, recall, F1 over collective attribution |

The current host report is intentionally a blocker, not a synthetic accuracy claim.
