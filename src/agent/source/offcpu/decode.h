#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "agent/source/offcpu/options.h"
#include "model/model.h"

namespace etrace::agent::offcpu {

// The raw structs mirror the BPF program's map values byte for byte.

inline constexpr int kCommLen = 16;
inline constexpr int kMaxStackDepth = 64;
inline constexpr uint8_t kBpfReasonPreempted = 1;
inline constexpr uint8_t kBpfReasonGpu = 2;

struct RawTrace {
	uint8_t trace_id[16];
	uint8_t span_id[8];
};

struct RawHop {
	uint32_t pid;
	uint32_t tgid;
	char comm[kCommLen];
	uint64_t wake_kstack;
	uint64_t wake_ustack;
	uint64_t wait_kstack;
	uint64_t wait_ustack;
	uint64_t wait_ns;
	RawTrace trace;
	uint8_t has_trace;
	uint8_t has_wait;
	uint8_t pad[6];
};
static_assert(sizeof(RawHop) == 96);

struct RawChain {
	uint32_t n;
	uint32_t pad;
	RawHop hops[kMaxHops];
};
static_assert(sizeof(RawChain) == 392);

struct RawKey {
	uint32_t pid;
	uint32_t tgid;
	uint64_t ustack;
	uint64_t kstack;
	uint64_t chain_id;
	char comm[kCommLen];
	uint8_t reason;
	uint8_t gpu_api;
	uint8_t gpu_op;
	uint8_t gpu_kernel_kind;
	uint8_t pad[4];
	uint64_t gpu_kernel;
};
static_assert(sizeof(RawKey) == 64);

struct RawVal {
	uint64_t total_ns;
	uint64_t count;
	uint64_t hop_wait_ns[kMaxHops];
};

struct RawEvent {
	uint64_t start_ns;
	uint64_t end_ns;
	uint32_t pid;
	uint32_t tgid;
	char comm[kCommLen];
	uint64_t kstack;
	uint64_t ustack;
	RawTrace trace;
	uint8_t reason;
	uint8_t gpu_api;
	uint8_t gpu_op;
	uint8_t gpu_kernel_kind;
	uint8_t pad[4];
	uint64_t gpu_kernel;
	uint64_t gpu_stream;
	uint32_t gpu_launches;
	uint32_t pad2;
	RawChain chain;
};
static_assert(sizeof(RawEvent) == 504);

struct RawStart {
	uint64_t ts;
	uint32_t tgid;
	uint32_t pad0;
	uint64_t ustack;
	uint64_t kstack;
	RawTrace trace;
	uint8_t reason;
	uint8_t has_trace;
	uint8_t in_gpu;
	uint8_t sampled;
	uint8_t pad[4];
};

struct RawStack {
	uint32_t len;
	uint32_t pad;
	uint64_t ips[kMaxStackDepth];
};

// RawHandleKey and RawKernelName are the key and value of the kernel_names
// map: a CUDA driver function handle of a process and its name.
struct RawHandleKey {
	uint32_t tgid;
	uint32_t pad;
	uint64_t handle;
};

struct RawKernelName {
	char name[128];
};

// RawGpuWait mirrors struct gpu_wait, an open host-blocking CUDA call.
struct RawGpuWait {
	uint64_t ts;
	uint64_t sp;
	uint64_t ustack;
	uint64_t obj;
	RawTrace trace;
	uint32_t tgid;
	uint8_t api;
	uint8_t depth;
	uint8_t has_trace;
	uint8_t has_chain;
	uint8_t argkind;
	uint8_t sampled;
	uint8_t pad[6];
	RawChain chain;
};

// RawLaunch mirrors struct launch, the last kernel queued on a stream or by
// a process.
struct RawLaunch {
	uint64_t kernel;
	uint64_t ts;
	uint32_t tid;
	uint32_t count;
	uint8_t kind;
	uint8_t pad[7];
};

inline constexpr uint8_t kGpuArgStream = 1;
inline constexpr uint8_t kGpuArgEvent = 2;

// StackLookup returns the instruction pointers of a stack id, or an empty
// list when the id is unknown.
using StackLookup = std::function<const std::vector<uint64_t>&(uint64_t id)>;
// KernelLookup returns the name of a CUDA driver function handle of a
// process, or "" when it was obtained before the agent attached.
using KernelLookup = std::function<std::string(uint32_t tgid, uint64_t handle)>;

model::OffCPUReason reason(uint8_t r);
std::string cstr(const char* b, size_t n);

model::OffCPUSample to_sample(const RawKey& k, const RawVal& v, const RawChain* chain, const StackLookup& lookup,
                              const KernelLookup& kernels = {});
std::vector<model::Hop> to_chain(const RawChain& c, const StackLookup& lookup);
// decode_event copies an event out of a ring buffer record; short records
// return nullopt.
std::optional<RawEvent> decode_event(std::span<const char> raw);
// to_event converts monotonic kernel timestamps to wall clock using the
// offset between CLOCK_REALTIME and CLOCK_MONOTONIC sampled by the caller.
model::OffCPUEvent to_event(const RawEvent& e, int64_t mono_to_wall, const StackLookup& lookup,
                            const KernelLookup& kernels = {});
// to_blocked describes a thread that is still off-CPU: its wait started at
// the recorded time and has not ended, so end is the observation time and
// there is no wakeup chain yet.
model::OffCPUEvent to_blocked(const RawStart& st, uint32_t tid, const std::string& comm, uint64_t now_mono,
                              int64_t mono_to_wall, const StackLookup& lookup);
// to_gpu_blocked describes a thread still inside a GPU wait, like
// to_blocked. launch is the kernel queued last on what the call waits for,
// if any, and stream the stream it waits on.
model::OffCPUEvent to_gpu_blocked(const RawGpuWait& w, uint32_t tid, const std::string& comm, uint64_t now_mono,
                                  int64_t mono_to_wall, const StackLookup& lookup, const KernelLookup& kernels,
                                  const RawLaunch* launch, uint64_t stream);
model::Stack join_stacks(const StackLookup& lookup, uint64_t kernel_id, uint64_t user_id);
std::vector<uint64_t> stack_ips(const RawStack& s);

}  // namespace etrace::agent::offcpu
