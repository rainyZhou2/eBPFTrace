#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "platform/time.h"

namespace etrace::model {

// gpu_kernel frames name the CUDA kernel a GPU wait was queued behind. With
// an address and no function, the address is the kernel's host stub in the
// task's process and resolves like a user frame.
enum class FrameKind : uint8_t { unknown = 0, kernel = 1, user = 2, gpu_kernel = 3 };

// kGpuKernelModule is the module of resolved gpu_kernel frames.
inline constexpr const char* kGpuKernelModule = "[gpu kernel]";

struct Frame {
	uint64_t address = 0;
	std::string function;
	std::string module;
	FrameKind kind = FrameKind::unknown;

	// symbol renders "module`function", or "[unknown]" for the function
	// when it is not resolved.
	std::string symbol() const;
};

// Stack is ordered from the innermost frame to the outermost.
using Stack = std::vector<Frame>;

std::vector<std::string> symbols(const Stack& s);

// PodRef identifies the Kubernetes pod and container a task runs in. It is
// empty outside Kubernetes or when the container runtime state is unreadable.
struct PodRef {
	std::string uid;
	std::string name;
	std::string ns;
	std::string container;

	bool empty() const { return uid.empty() && name.empty() && ns.empty() && container.empty(); }
	bool operator==(const PodRef&) const = default;
};

struct Task {
	uint32_t pid = 0;
	uint32_t tid = 0;
	std::string comm;
	std::string container_id;
	PodRef pod;

	bool empty() const { return pid == 0 && tid == 0 && comm.empty(); }
};

// gpu is time a thread spent in a CUDA call that waits for GPU work, whether
// the driver spun on the CPU or slept.
enum class OffCPUReason : uint8_t { unknown = 0, blocked = 1, preempted = 2, gpu = 3 };

const char* to_string(OffCPUReason r);

// Evidence records how a hop entered the chain. observed means the wakeup
// was seen in sched_waking; inferred means it was linked by time overlap.
enum class Evidence : uint8_t { unknown = 0, observed = 1, inferred = 2 };

const char* to_string(Evidence e);

struct TraceContext {
	std::array<uint8_t, 16> trace_id{};
	std::array<uint8_t, 8> span_id{};

	bool valid() const { return trace_id != std::array<uint8_t, 16>{}; }
	std::string trace_id_hex() const;
	std::string span_id_hex() const;
	bool operator==(const TraceContext&) const = default;
};

// parse_trace_context accepts a 32-digit hex trace id and an optional
// 16-digit hex span id.
std::optional<TraceContext> parse_trace_context(std::string_view trace_id, std::string_view span_id);

// Hop is one link of a wakeup causal chain. Hop 0 is the task that woke the
// blocked thread; hop i+1 is the task that woke hop i while hop i was itself
// blocked during the original wait.
struct Hop {
	Task task;
	Stack wake_stack;
	Stack wait_stack;
	uint64_t wait_ns = 0;
	TraceContext trace;
	Evidence wake_evidence = Evidence::unknown;
	Evidence wait_evidence = Evidence::unknown;

	bool waited() const { return !wait_stack.empty() || wait_ns > 0; }
};

struct OffCPUSample {
	Task task;
	Stack stack;
	std::vector<Hop> chain;
	uint64_t blocked_ns = 0;
	uint64_t count = 0;
	OffCPUReason reason = OffCPUReason::unknown;
};

// GPUWait details a GPU wait event: the CUDA stream it waited on (0 for the
// default stream and context-wide waits) and the kernels queued on it since
// the previous wait on it.
struct GPUWait {
	uint64_t stream = 0;
	uint32_t launches = 0;
	bool operator==(const GPUWait&) const = default;
};

// OffCPUEvent is a single off-CPU interval, kept unaggregated so it can be
// attributed to the request that was active on the thread.
struct OffCPUEvent {
	Task task;
	Stack stack;
	std::vector<Hop> chain;
	Time start{};
	Time end{};
	OffCPUReason reason = OffCPUReason::unknown;
	TraceContext trace;
	GPUWait gpu;

	Duration duration() const { return end - start; }
};

struct Resource {
	std::string node;
	std::string agent_id;
	std::string agent_version;
	std::map<std::string, std::string> labels;
};

struct Batch {
	std::string id;
	Resource resource;
	Time window_start{};
	Time window_end{};
	std::vector<OffCPUSample> offcpu;
	std::vector<OffCPUEvent> events;

	bool empty() const { return offcpu.empty() && events.empty(); }
};

}  // namespace etrace::model
