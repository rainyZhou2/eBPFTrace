# Benchmarks

- [overhead](overhead/README.md): real off-CPU capture overhead on a pthread mutex/futex workload.
- [nccl](nccl/README.md): NCCL collective attribution accuracy protocol; blocked until a real GPU host is available.

Names used across both reports: `baseline` (no eTrace), `etrace` (with eTrace), `raw wait rate`, `accepted event rate`, `ring-buffer drops`, `queue saturation`, and the `nccl*` collective names.
