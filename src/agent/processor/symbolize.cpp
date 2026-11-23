#include "agent/processor/symbolize.h"

#include <array>
#include <string_view>

namespace etrace::agent::symbolize {

void decode_fields(config::Fields& f, Options& o) {
	f("process_cache", o.process_cache)("process_ttl", o.process_ttl)("elf_cache", o.elf_cache)("debuginfod",
	                                                                                            o.debuginfod);
}

Processor::Processor(std::unique_ptr<symbol::Resolver> r, std::unique_ptr<symbol::Debuginfod> d)
    : debuginfod_(std::move(d)), r_(std::move(r)) {}

std::unique_ptr<Processor> Processor::from_options(const Options& o, const Deps& deps) {
	symbol::Config rc;
	rc.proc_root = deps.proc_root;
	rc.process_cache = o.process_cache;
	rc.process_ttl = o.process_ttl;
	rc.elf_cache = o.elf_cache;
	std::unique_ptr<symbol::Debuginfod> d;
	if (!o.debuginfod.urls.empty()) {
		d = std::make_unique<symbol::Debuginfod>(o.debuginfod, deps.log, deps.registry);
		rc.debug = d.get();
	}
	auto r = std::make_unique<symbol::ProcResolver>(rc);
	return std::make_unique<Processor>(std::move(r), std::move(d));
}

void Processor::process(model::Batch& b) {
	auto chain = [this](std::vector<model::Hop>& c) {
		for (auto& h : c) {
			resolve(h.task.pid, h.wake_stack);
			resolve(h.task.pid, h.wait_stack);
			label_gpu_interrupt(h);
		}
	};
	for (auto& s : b.offcpu) {
		resolve(s.task.pid, s.stack);
		chain(s.chain);
	}
	for (auto& e : b.events) {
		resolve(e.task.pid, e.stack);
		chain(e.chain);
	}
}

namespace {

constexpr std::array<std::string_view, 6> kTracingPrefixes = {"bpf_prog_",    "bpf_trace_run",  "__bpf_trace_",
                                                              "__traceiter_", "trace_call_bpf", "perf_trace_"};

bool tracing_frame(const model::Frame& f) {
	if (f.kind != model::FrameKind::kernel) {
		return false;
	}
	for (auto p : kTracingPrefixes) {
		if (f.function.starts_with(p)) {
			return true;
		}
	}
	return false;
}

}  // namespace

// label_gpu_interrupt names a hard IRQ wakeup raised by the NVIDIA driver
// "[gpu]": the GPU signalled that the work the thread waited for is done.
void Processor::label_gpu_interrupt(model::Hop& h) {
	if (h.task.comm != "[hardirq]") {
		return;
	}
	for (const auto& f : h.wake_stack) {
		if (f.kind == model::FrameKind::kernel && f.function.starts_with("nvidia_isr")) {
			h.task.comm = "[gpu]";
			return;
		}
	}
}

void Processor::resolve(uint32_t pid, model::Stack& st) {
	for (auto& f : st) {
		if (!f.function.empty()) {
			continue;
		}
		symbol::Resolved r;
		if (f.kind == model::FrameKind::kernel) {
			r = r_->kernel(f.address);
		} else if (f.kind == model::FrameKind::user) {
			r = r_->user(pid, f.address);
		} else if (f.kind == model::FrameKind::gpu_kernel && f.address != 0) {
			// The host stub of a CUDA kernel carries the kernel's name.
			r = r_->user(pid, f.address);
			r.module = model::kGpuKernelModule;
		} else {
			continue;
		}
		f.function = std::move(r.function);
		f.module = std::move(r.module);
	}
	// Drop the innermost kernel frames that belong to the tracing machinery
	// that captured the stack rather than to the traced code.
	size_t i = 0;
	while (i < st.size() && tracing_frame(st[i])) {
		i++;
	}
	st.erase(st.begin(), st.begin() + static_cast<std::ptrdiff_t>(i));
}

}  // namespace etrace::agent::symbolize
