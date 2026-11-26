#include "collector/service.h"

#include <chrono>

namespace etrace::collector {

void decode_fields(config::Fields& f, DedupConfig& c) {
	f("size", c.size)("ttl", c.ttl);
}

void decode_fields(config::Fields& f, Limits& l) {
	f("max_samples_per_batch", l.max_samples_per_batch)("max_events_per_batch", l.max_events_per_batch)(
	        "max_frames_per_stack", l.max_frames_per_stack)("max_chain_hops", l.max_chain_hops);
}

void Limits::validate() const {
	if (max_samples_per_batch < 0 || max_events_per_batch < 0 || max_frames_per_stack < 0 || max_chain_hops < 0) {
		throw Error("collector limits must not be negative");
	}
}

Service::Service(storage::Writer& w, DedupConfig dedup, Limits limits, telemetry::Registry& reg, log::Logger log)
    : writer_(w),
      log_(std::move(log)),
      seen_(static_cast<size_t>(std::max(dedup.size, 1)), dedup.ttl),
      limits_(limits),
      batches_(reg.counter_family("etrace_collector_batches_total")),
      samples_(reg.counter("etrace_collector_samples_total")),
      latency_(reg.histogram("etrace_collector_write_seconds", telemetry::exponential_buckets(0.001, 2, 14))) {}

bool Service::ingest(const model::Batch& b) {
	try {
		validate(b);
	} catch (const InvalidBatch&) {
		batches_.Add({{"result", "invalid"}}).Increment();
		throw;
	}
	if (seen_.get(b.id)) {
		batches_.Add({{"result", "duplicate"}}).Increment();
		return true;
	}
	auto start = std::chrono::steady_clock::now();
	auto observe = [&] {
		latency_.Observe(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
	};
	try {
		writer_.write_batch(b);
	} catch (const storage::DuplicateError&) {
		observe();
		seen_.add(b.id, true);
		batches_.Add({{"result", "duplicate"}}).Increment();
		return true;
	} catch (const std::exception& e) {
		observe();
		batches_.Add({{"result", "error"}}).Increment();
		log_.warn("write batch failed", {{"batch", b.id}, {"node", b.resource.node}, {"err", e}});
		throw;
	}
	observe();
	seen_.add(b.id, true);
	batches_.Add({{"result", "ok"}}).Increment();
	samples_.Increment(static_cast<double>(b.offcpu.size() + b.events.size()));
	return false;
}

void Service::validate(const model::Batch& b) const {
	const auto& l = limits_;
	if (l.max_samples_per_batch > 0 && b.offcpu.size() > static_cast<size_t>(l.max_samples_per_batch)) {
		throw InvalidBatch("too many samples");
	}
	if (l.max_events_per_batch > 0 && b.events.size() > static_cast<size_t>(l.max_events_per_batch)) {
		throw InvalidBatch("too many events");
	}
	for (const auto& s : b.offcpu) {
		check_stacks(s.stack, s.chain);
	}
	for (const auto& e : b.events) {
		check_stacks(e.stack, e.chain);
	}
}

void Service::check_stacks(const model::Stack& st, const std::vector<model::Hop>& chain) const {
	const auto& l = limits_;
	if (l.max_chain_hops > 0 && chain.size() > static_cast<size_t>(l.max_chain_hops)) {
		throw InvalidBatch("chain too long");
	}
	if (l.max_frames_per_stack <= 0) {
		return;
	}
	auto max = static_cast<size_t>(l.max_frames_per_stack);
	bool too_deep = st.size() > max;
	for (const auto& h : chain) {
		too_deep = too_deep || h.wake_stack.size() > max || h.wait_stack.size() > max;
	}
	if (too_deep) {
		throw InvalidBatch("stack too deep");
	}
}

}  // namespace etrace::collector
