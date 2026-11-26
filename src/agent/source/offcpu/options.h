#pragma once

#include <string>
#include <vector>

#include "agent/policy.h"
#include "platform/config.h"
#include "platform/time.h"

namespace etrace::agent::offcpu {

inline constexpr int kMaxHops = 4;

inline constexpr const char* kUnwindAuto = "auto";
inline constexpr const char* kUnwindDwarf = "dwarf";
inline constexpr const char* kUnwindFramePointer = "frame_pointer";

// kMaxUnwindRows is the largest table the BPF row search can cover.
inline constexpr int kMaxUnwindRows = 1 << 24;

struct TraceContextOptions {
	bool enabled = true;
	std::vector<std::string> library_patterns = {"libetrace_ctx"};
	std::vector<std::string> paths;
	std::string symbol = "etrace_register_tls";
	Duration scan_interval = 10s;
};

// UnwindOptions selects how user stacks are walked. dwarf uses .eh_frame
// tables and needs Linux 5.17; auto uses it when the kernel accepts the
// program and otherwise falls back to frame pointers.
struct UnwindOptions {
	std::string mode = kUnwindAuto;
	int max_rows = 1 << 20;
	int max_processes = 4096;
	Duration refresh_interval = 1s;
};

// GPUOptions controls GPU wait timing: uprobes on the host-blocking calls
// of matching CUDA libraries and on NCCL collectives. Needs Linux 5.15.
// launches also probes kernel launches to name the kernel a wait was queued
// behind; that probe fires on every launch, so it is detached for good when
// the host-wide launch rate exceeds max_launch_rate per second.
struct GPUOptions {
	bool enabled = true;
	std::vector<std::string> library_patterns = {"libcuda.so", "libcudart.so", "libnccl.so"};
	std::vector<std::string> paths;
	Duration min_wait = 1ms;
	Duration scan_interval = 10s;
	bool launches = true;
	double max_launch_rate = 20000;
};

struct Options {
	uint32_t target_pid = 0;
	Duration min_block = 1ms;
	Duration max_block{0};
	bool include_preempted = false;
	bool user_stacks = true;
	int chain_depth = kMaxHops;
	Duration slow_event{0};
	int max_events_per_interval = 20000;
	Duration drain_grace = 5ms;
	TraceContextOptions trace_context;
	UnwindOptions unwind;
	GPUOptions gpu;
	policy::Config policy;

	void validate() const;
};

void decode_fields(config::Fields& f, TraceContextOptions& o);
void decode_fields(config::Fields& f, UnwindOptions& o);
void decode_fields(config::Fields& f, GPUOptions& o);
void decode_fields(config::Fields& f, Options& o);

}  // namespace etrace::agent::offcpu
