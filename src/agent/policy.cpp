#include "agent/policy.h"

#include <time.h>
#include <algorithm>
#include <format>

#include "platform/error.h"
#include "platform/sync.h"

namespace etrace::agent::policy {

namespace {

double cpu_ratio(const Signals& previous, const Signals& current, double seconds) {
	auto delta = current.cpu_seconds - previous.cpu_seconds;
	return delta > 0 ? delta / seconds : 0;
}

uint64_t delta(uint64_t previous, uint64_t current) {
	return current >= previous ? current - previous : 0;
}

double rate(uint64_t previous, uint64_t current, double seconds) {
	return static_cast<double>(delta(previous, current)) / seconds;
}

double bpf_ratio(const Signals& previous, const Signals& current, double seconds) {
	return static_cast<double>(delta(previous.bpf_run_time_ns, current.bpf_run_time_ns)) / (seconds * 1e9);
}

uint32_t clamp_events(uint32_t value, const Config& cfg) {
	return std::clamp(value, cfg.min_events_per_interval, cfg.max_events_per_interval);
}

uint32_t clamp_depth(uint32_t value, const Config& cfg) {
	return std::clamp(value, cfg.min_unwind_depth, cfg.max_unwind_depth);
}

uint32_t clamp_sample(uint32_t value, const Config& cfg) {
	return std::clamp(value, cfg.min_sample_percent, cfg.max_sample_percent);
}

}  // namespace

void decode_fields(config::Fields& f, Config& o) {
	f("enabled", o.enabled)("interval", o.interval)("cooldown", o.cooldown)("max_events_per_interval",
	                                                                        o.max_events_per_interval)(
	        "min_events_per_interval", o.min_events_per_interval)("max_unwind_depth", o.max_unwind_depth)(
	        "min_unwind_depth", o.min_unwind_depth)("max_sample_percent", o.max_sample_percent)(
	        "min_sample_percent", o.min_sample_percent)("target_event_rate", o.target_event_rate)(
	        "max_cpu_ratio", o.max_cpu_ratio)("max_drop_rate", o.max_drop_rate)("max_bpf_ratio", o.max_bpf_ratio)(
	        "max_ringbuf_drop_rate", o.max_ringbuf_drop_rate)("max_queue_saturation", o.max_queue_saturation)(
	        "max_processes", o.max_processes)("allow_mode_switch", o.allow_mode_switch);
}

void Config::validate() const {
	if (interval < 100ms) {
		throw Error("policy.interval must be at least 100ms");
	}
	if (cooldown < 0s) {
		throw Error("policy.cooldown must not be negative");
	}
	if (max_events_per_interval == 0 || min_events_per_interval == 0 ||
	    min_events_per_interval > max_events_per_interval) {
		throw Error("policy event bounds must be positive and ordered");
	}
	if (max_unwind_depth == 0 || max_unwind_depth > kMaxUnwindDepth || min_unwind_depth == 0 ||
	    min_unwind_depth > max_unwind_depth) {
		throw Error(std::format("policy unwind depth must be between 1 and {}", kMaxUnwindDepth));
	}
	if (max_sample_percent > 100 || min_sample_percent > max_sample_percent) {
		throw Error("policy sample bounds must be ordered percentages");
	}
	if (target_event_rate <= 0 || max_cpu_ratio < 0 || max_drop_rate < 0 || max_bpf_ratio < 0 ||
	    max_ringbuf_drop_rate < 0 || max_queue_saturation <= 0 || max_queue_saturation > 1) {
		throw Error("policy thresholds must be non-negative and target_event_rate positive");
	}
}

Decision Planner::decide(const Signals& previous, const Signals& current, Duration elapsed,
                         const Decision& decision) const {
	double seconds = std::max(std::chrono::duration<double>(elapsed).count(), 1e-6);
	double cpu = cpu_ratio(previous, current, seconds);
	double raw_rate = rate(previous.raw_waits, current.raw_waits, seconds);
	double drops = rate(previous.bpf_drops, current.bpf_drops, seconds);
	double bpf = bpf_ratio(previous, current, seconds);
	double ring = rate(previous.ringbuf_drops, current.ringbuf_drops, seconds);
	auto capacity_delta = delta(previous.capacity_drops, current.capacity_drops);
	bool process_pressure = cfg_.max_processes != 0 && current.processes > cfg_.max_processes;
	bool pressure = cpu > cfg_.max_cpu_ratio || raw_rate > cfg_.target_event_rate || drops > cfg_.max_drop_rate ||
	                bpf > cfg_.max_bpf_ratio || ring > cfg_.max_ringbuf_drop_rate || capacity_delta != 0 ||
	                current.queue_saturation > cfg_.max_queue_saturation || process_pressure;
	bool relaxed = !pressure && cpu < cfg_.max_cpu_ratio * 0.5 && raw_rate < cfg_.target_event_rate * 0.5 &&
	               drops == 0 && bpf < cfg_.max_bpf_ratio * 0.5 && ring == 0 && capacity_delta == 0 &&
	               current.queue_saturation < cfg_.max_queue_saturation * 0.5 && !process_pressure;

	Decision next = decision;
	if (pressure) {
		next.max_events_per_interval =
		        std::max(cfg_.min_events_per_interval, decision.max_events_per_interval * 3 / 4);
		next.unwind_depth = std::max(cfg_.min_unwind_depth, decision.unwind_depth * 3 / 4);
		next.sample_percent = std::max(cfg_.min_sample_percent, decision.sample_percent * 3 / 4);
		if (cfg_.allow_mode_switch && current.dwarf_available && decision.unwind_mode != "frame_pointer") {
			next.unwind_mode = "frame_pointer";
		}
	} else if (relaxed) {
		next.max_events_per_interval =
		        std::min(cfg_.max_events_per_interval,
		                 decision.max_events_per_interval + std::max(1u, cfg_.max_events_per_interval / 4));
		next.unwind_depth = std::min(cfg_.max_unwind_depth, decision.unwind_depth + 8);
		next.sample_percent = std::min(cfg_.max_sample_percent, decision.sample_percent + 10);
		if (cfg_.allow_mode_switch && current.dwarf_available && decision.unwind_mode == "frame_pointer") {
			next.unwind_mode = "dwarf";
		}
	}
	next.max_events_per_interval = clamp_events(next.max_events_per_interval, cfg_);
	next.unwind_depth = clamp_depth(next.unwind_depth, cfg_);
	next.sample_percent = clamp_sample(next.sample_percent, cfg_);
	return next;
}

double process_cpu_seconds() {
	timespec ts{};
	if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) {
		return 0;
	}
	return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

Controller::Controller(Config cfg, Decision initial, Sampler sampler, Sink sink, log::Logger log)
    : cfg_(std::move(cfg)),
      current_(std::move(initial)),
      planner_(cfg_),
      sampler_(std::move(sampler)),
      sink_(std::move(sink)),
      log_(std::move(log).with({{"component", "policy"}})) {}

ApplyResult Controller::apply(const Decision& decision) {
	try {
		return sink_(decision);
	} catch (const std::exception& e) {
		log_.warn("policy apply failed", {{"err", e}});
		return {false, current_};
	}
}

void Controller::run(std::stop_token st) {
	auto initial = apply(current_);
	if (initial.success) {
		current_ = initial.actual;
	} else {
		log_.warn("initial policy apply failed");
	}
	Signals previous;
	try {
		previous = sampler_();
	} catch (const std::exception& e) {
		log_.warn("policy initial sample failed", {{"err", e}});
		return;
	}
	auto previous_at = std::chrono::steady_clock::now();
	auto last_change = previous_at - cfg_.cooldown;
	while (!st.stop_requested()) {
		if (!sleep_for(st, cfg_.interval)) {
			break;
		}
		auto now = std::chrono::steady_clock::now();
		Signals current;
		try {
			current = sampler_();
		} catch (const std::exception& e) {
			log_.warn("policy sample failed", {{"err", e}});
			previous_at = now;
			continue;
		}
		auto next = planner_.decide(previous, current, now - previous_at, current_);
		if (next != current_ && now - last_change >= cfg_.cooldown) {
			auto result = apply(next);
			if (!result.success) {
				log_.warn("policy change was not applied; retrying next interval");
				previous = std::move(current);
				previous_at = now;
				continue;
			}
			current_ = std::move(result.actual);
			log_.info("policy adjusted", {{"max_events", current_.max_events_per_interval},
			                              {"unwind_depth", current_.unwind_depth},
			                              {"sample_percent", current_.sample_percent},
			                              {"unwind_mode", current_.unwind_mode}});
			last_change = now;
		}
		previous = std::move(current);
		previous_at = now;
	}
}

}  // namespace etrace::agent::policy
