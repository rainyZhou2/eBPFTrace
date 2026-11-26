#pragma once

#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <utility>

#include "platform/config.h"
#include "platform/log.h"
#include "platform/time.h"

namespace etrace::agent::policy {

inline constexpr uint32_t kMaxUnwindDepth = 64;

struct Config {
	bool enabled = false;
	Duration interval = 10s;
	Duration cooldown = 30s;
	uint32_t max_events_per_interval = 20000;
	uint32_t min_events_per_interval = 1000;
	uint32_t max_unwind_depth = 64;
	uint32_t min_unwind_depth = 16;
	uint32_t max_sample_percent = 100;
	uint32_t min_sample_percent = 10;
	double target_event_rate = 5000;
	double max_cpu_ratio = 0.25;
	double max_drop_rate = 0;
	double max_bpf_ratio = 0.20;
	double max_ringbuf_drop_rate = 0;
	double max_queue_saturation = 0.90;
	uint32_t max_processes = 2048;
	bool allow_mode_switch = true;

	void validate() const;
};

void decode_fields(config::Fields& f, Config& o);

struct Signals {
	double cpu_seconds = 0;
	uint64_t raw_waits = 0;
	uint64_t accepted_events = 0;
	uint64_t rejected_events = 0;
	uint64_t sampled_waits = 0;
	uint64_t capacity_drops = 0;
	uint64_t decode_drops = 0;
	uint64_t ringbuf_drops = 0;
	uint64_t bpf_drops = 0;
	uint64_t bpf_run_time_ns = 0;
	uint64_t bpf_run_cnt = 0;
	double queue_saturation = 0;
	uint32_t processes = 0;
	std::string unwind_mode;
	bool dwarf_available = false;
};

struct Decision {
	uint32_t max_events_per_interval = 20000;
	uint32_t unwind_depth = 64;
	uint32_t sample_percent = 100;
	std::string unwind_mode;

	bool operator==(const Decision&) const = default;
};

struct ApplyResult {
	bool success = false;
	Decision actual;
};

class Planner {
public:
	explicit Planner(Config cfg) : cfg_(std::move(cfg)) {}

	Decision decide(const Signals& previous, const Signals& current, Duration elapsed,
	                const Decision& decision) const;

private:
	Config cfg_;
};

double process_cpu_seconds();

class Controller {
public:
	using Sampler = std::function<Signals()>;
	using Sink = std::function<ApplyResult(const Decision&)>;

	Controller(Config cfg, Decision initial, Sampler sampler, Sink sink, log::Logger log);

	void run(std::stop_token st);

private:
	ApplyResult apply(const Decision& decision);

	Config cfg_;
	Decision current_;
	Planner planner_;
	Sampler sampler_;
	Sink sink_;
	log::Logger log_;
};

}  // namespace etrace::agent::policy
