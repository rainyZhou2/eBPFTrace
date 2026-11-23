#include "agent/source/offcpu/options.h"

#include <format>

#include "platform/error.h"

namespace etrace::agent::offcpu {

void decode_fields(config::Fields& f, TraceContextOptions& o) {
	f("enabled", o.enabled)("library_patterns", o.library_patterns)("paths", o.paths)("symbol", o.symbol)(
	        "scan_interval", o.scan_interval);
}

void decode_fields(config::Fields& f, UnwindOptions& o) {
	f("mode", o.mode)("max_rows", o.max_rows)("max_processes", o.max_processes)("refresh_interval",
	                                                                            o.refresh_interval);
}

void decode_fields(config::Fields& f, GPUOptions& o) {
	f("enabled", o.enabled)("library_patterns", o.library_patterns)("paths", o.paths)("min_wait", o.min_wait)(
	        "scan_interval", o.scan_interval)("launches", o.launches)("max_launch_rate", o.max_launch_rate);
}

void decode_fields(config::Fields& f, Options& o) {
	f("target_pid", o.target_pid)("min_block", o.min_block)("max_block", o.max_block)(
	        "include_preempted", o.include_preempted)("user_stacks", o.user_stacks)("chain_depth", o.chain_depth)(
	        "slow_event", o.slow_event)("max_events_per_interval", o.max_events_per_interval)(
	        "drain_grace", o.drain_grace)("trace_context", o.trace_context)("unwind", o.unwind)("gpu", o.gpu);
}

void Options::validate() const {
	if (min_block < Duration(0)) {
		throw Error("min_block must not be negative");
	}
	if (max_block != Duration(0) && max_block < min_block) {
		throw Error("max_block must be greater than min_block");
	}
	if (chain_depth < 0 || chain_depth > kMaxHops) {
		throw Error("chain_depth must be between 0 and 4");
	}
	if (slow_event < Duration(0)) {
		throw Error("slow_event must not be negative");
	}
	if (max_events_per_interval <= 0) {
		throw Error("max_events_per_interval must be positive");
	}
	if (unwind.mode != kUnwindAuto && unwind.mode != kUnwindDwarf && unwind.mode != kUnwindFramePointer) {
		throw Error(std::format("unwind.mode must be {}, {} or {}", kUnwindAuto, kUnwindDwarf,
		                        kUnwindFramePointer));
	}
	if (unwind.mode != kUnwindFramePointer) {
		if (unwind.max_rows <= 0 || unwind.max_rows > kMaxUnwindRows) {
			throw Error(std::format("unwind.max_rows must be between 1 and {}", kMaxUnwindRows));
		}
		if (unwind.max_processes <= 0) {
			throw Error("unwind.max_processes must be positive");
		}
		if (unwind.refresh_interval < 100ms) {
			throw Error("unwind.refresh_interval must be at least 100ms");
		}
	}
	if (trace_context.enabled) {
		if (trace_context.symbol.empty()) {
			throw Error("trace_context.symbol is required");
		}
		if (trace_context.scan_interval < 1s) {
			throw Error("trace_context.scan_interval must be at least 1s");
		}
	}
	if (gpu.enabled) {
		if (gpu.min_wait < Duration(0)) {
			throw Error("gpu.min_wait must not be negative");
		}
		if (gpu.scan_interval < 1s) {
			throw Error("gpu.scan_interval must be at least 1s");
		}
		if (gpu.launches && gpu.max_launch_rate <= 0) {
			throw Error("gpu.max_launch_rate must be positive");
		}
	}
}

}  // namespace etrace::agent::offcpu
