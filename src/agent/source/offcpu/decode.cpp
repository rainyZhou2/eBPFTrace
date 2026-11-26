#include "agent/source/offcpu/decode.h"

#include <algorithm>

#include "agent/source/offcpu/gpu.h"

namespace etrace::agent::offcpu {

model::OffCPUReason reason(uint8_t r) {
	switch (r) {
		case kBpfReasonPreempted:
			return model::OffCPUReason::preempted;
		case kBpfReasonGpu:
			return model::OffCPUReason::gpu;
		default:
			return model::OffCPUReason::blocked;
	}
}

std::string cstr(const char* b, size_t n) {
	const void* z = std::memchr(b, 0, n);
	return std::string(b, z ? static_cast<const char*>(z) - b : n);
}

namespace {

model::TraceContext trace_of(const RawTrace& t) {
	model::TraceContext tc;
	std::memcpy(tc.trace_id.data(), t.trace_id, 16);
	std::memcpy(tc.span_id.data(), t.span_id, 8);
	return tc;
}

// wait_stack is the stack of a sample or event; GPU waits are named by
// synthetic innermost frames.
model::Stack wait_stack(const StackLookup& lookup, uint8_t r, uint64_t kstack, uint64_t ustack, const gpu::Wait& w,
                        uint32_t tgid, const KernelLookup& kernels) {
	auto st = join_stacks(lookup, kstack, ustack);
	if (r == kBpfReasonGpu) {
		std::string name;
		if (w.kernel_kind == gpu::kKernelFunc && kernels) {
			name = kernels(tgid, w.kernel);
		}
		auto g = gpu::frames(w, name);
		st.insert(st.begin(), g.begin(), g.end());
	}
	return st;
}

}  // namespace

model::Stack join_stacks(const StackLookup& lookup, uint64_t kernel_id, uint64_t user_id) {
	model::Stack out;
	if (kernel_id != 0) {
		for (auto ip : lookup(kernel_id)) {
			out.push_back(model::Frame{ip, "", "", model::FrameKind::kernel});
		}
	}
	if (user_id != 0) {
		for (auto ip : lookup(user_id)) {
			out.push_back(model::Frame{ip, "", "", model::FrameKind::user});
		}
	}
	return out;
}

std::vector<model::Hop> to_chain(const RawChain& c, const StackLookup& lookup) {
	auto n = std::min<uint32_t>(c.n, kMaxHops);
	std::vector<model::Hop> out;
	out.reserve(n);
	for (uint32_t i = 0; i < n; i++) {
		const auto& h = c.hops[i];
		model::Hop hop;
		hop.task = model::Task{h.tgid, h.pid, cstr(h.comm, kCommLen), "", {}};
		hop.wake_stack = join_stacks(lookup, h.wake_kstack, h.wake_ustack);
		hop.wake_evidence = i == 0 ? model::Evidence::observed : model::Evidence::inferred;
		if (h.has_wait) {
			hop.wait_stack = join_stacks(lookup, h.wait_kstack, h.wait_ustack);
			hop.wait_ns = h.wait_ns;
			hop.wait_evidence = model::Evidence::inferred;
		}
		if (h.has_trace) {
			hop.trace = trace_of(h.trace);
		}
		out.push_back(std::move(hop));
	}
	return out;
}

model::OffCPUSample to_sample(const RawKey& k, const RawVal& v, const RawChain* chain, const StackLookup& lookup,
                              const KernelLookup& kernels) {
	model::OffCPUSample s;
	s.task = model::Task{k.tgid, k.pid, cstr(k.comm, kCommLen), "", {}};
	s.stack = wait_stack(lookup, k.reason, k.kstack, k.ustack,
	                     gpu::Wait{k.gpu_api, k.gpu_op, k.gpu_kernel_kind, k.gpu_kernel}, k.tgid, kernels);
	s.blocked_ns = v.total_ns;
	s.count = v.count;
	s.reason = reason(k.reason);
	if (chain) {
		s.chain = to_chain(*chain, lookup);
		for (size_t i = 0; i < s.chain.size(); i++) {
			if (s.chain[i].waited()) {
				s.chain[i].wait_ns = v.hop_wait_ns[i];
			}
		}
	}
	return s;
}

std::optional<RawEvent> decode_event(std::span<const char> raw) {
	if (raw.size() < sizeof(RawEvent)) {
		return std::nullopt;
	}
	RawEvent e;
	std::memcpy(&e, raw.data(), sizeof e);
	return e;
}

model::OffCPUEvent to_event(const RawEvent& e, int64_t mono_to_wall, const StackLookup& lookup,
                            const KernelLookup& kernels) {
	model::OffCPUEvent out;
	out.task = model::Task{e.tgid, e.pid, cstr(e.comm, kCommLen), "", {}};
	out.stack = wait_stack(lookup, e.reason, e.kstack, e.ustack,
	                       gpu::Wait{e.gpu_api, e.gpu_op, e.gpu_kernel_kind, e.gpu_kernel}, e.tgid, kernels);
	out.chain = to_chain(e.chain, lookup);
	out.start = from_unix_nano(static_cast<int64_t>(e.start_ns) + mono_to_wall);
	out.end = from_unix_nano(static_cast<int64_t>(e.end_ns) + mono_to_wall);
	out.reason = reason(e.reason);
	out.trace = trace_of(e.trace);
	if (e.reason == kBpfReasonGpu) {
		out.gpu = model::GPUWait{e.gpu_stream, e.gpu_launches};
	}
	return out;
}

model::OffCPUEvent to_blocked(const RawStart& st, uint32_t tid, const std::string& comm, uint64_t now_mono,
                              int64_t mono_to_wall, const StackLookup& lookup) {
	model::OffCPUEvent e;
	e.task = model::Task{st.tgid, tid, comm, "", {}};
	e.stack = join_stacks(lookup, st.kstack, st.ustack);
	e.start = from_unix_nano(static_cast<int64_t>(st.ts) + mono_to_wall);
	e.end = from_unix_nano(static_cast<int64_t>(now_mono) + mono_to_wall);
	e.reason = reason(st.reason);
	if (st.has_trace) {
		e.trace = trace_of(st.trace);
	}
	return e;
}

model::OffCPUEvent to_gpu_blocked(const RawGpuWait& w, uint32_t tid, const std::string& comm, uint64_t now_mono,
                                  int64_t mono_to_wall, const StackLookup& lookup, const KernelLookup& kernels,
                                  const RawLaunch* launch, uint64_t stream) {
	model::OffCPUEvent e;
	e.task = model::Task{w.tgid, tid, comm, "", {}};
	gpu::Wait g{w.api, 0, 0, 0};
	if (launch) {
		g.kernel_kind = launch->kind;
		g.kernel = launch->kernel;
		e.gpu.launches = launch->count;
	}
	e.stack = wait_stack(lookup, kBpfReasonGpu, 0, w.ustack, g, w.tgid, kernels);
	e.start = from_unix_nano(static_cast<int64_t>(w.ts) + mono_to_wall);
	e.end = from_unix_nano(static_cast<int64_t>(now_mono) + mono_to_wall);
	e.reason = model::OffCPUReason::gpu;
	e.gpu.stream = stream;
	if (w.has_trace) {
		e.trace = trace_of(w.trace);
	}
	return e;
}

std::vector<uint64_t> stack_ips(const RawStack& s) {
	auto n = std::min<uint32_t>(s.len, kMaxStackDepth);
	return {s.ips, s.ips + n};
}

}  // namespace etrace::agent::offcpu
