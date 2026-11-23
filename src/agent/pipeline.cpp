#include "agent/pipeline.h"

#include <format>
#include <thread>

#include "etrace/v1/ingest.pb.h"
#include "platform/hex.h"
#include "wire/wire.h"

namespace etrace::agent {

Pipeline::Pipeline(PipelineConfig c, telemetry::Registry& reg)
    : c_(std::move(c)),
      log_(c_.log),
      collected_(reg.counter("etrace_agent_batches_collected_total")),
      exported_(reg.counter_family("etrace_agent_batches_exported_total")) {
	auto* q = c_.queue;
	reg.gauge_func("etrace_agent_queue_batches", "", [q] { return static_cast<double>(q->size()); });
	reg.gauge_func("etrace_agent_queue_bytes", "", [q] { return static_cast<double>(q->bytes()); });
	reg.counter_func("etrace_agent_queue_evicted_total", "", [q] { return static_cast<double>(q->evicted()); });
}

Pipeline::~Pipeline() = default;

void Pipeline::run(std::stop_token st) {
	for (auto& s : c_.sources) {
		try {
			s->start();
		} catch (const std::exception& e) {
			throw Error(std::format("start source {}: {}", s->name(), e.what()));
		}
		log_.info("source started", {{"source", s->name()}});
	}
	std::jthread shipper([this, st] { ship(st); });

	auto window_start = now();
	auto next = std::chrono::steady_clock::now() + c_.interval;
	std::mutex mu;
	std::condition_variable_any cv;
	while (true) {
		bool stopped;
		{
			std::unique_lock lock(mu);
			stopped = cv.wait_until(lock, st, next, [] { return false; }) || st.stop_requested();
		}
		auto end = now();
		collect(window_start, end);
		if (stopped) {
			break;
		}
		window_start = end;
		next += c_.interval;
		if (next < std::chrono::steady_clock::now()) {
			next = std::chrono::steady_clock::now() + c_.interval;
		}
	}
	shipper.join();
}

void Pipeline::collect(Time start, Time end) {
	model::Batch b;
	b.id = random_hex(16);
	b.resource = c_.resource;
	b.window_start = start;
	b.window_end = end;
	for (auto& s : c_.sources) {
		try {
			s->collect(b);
		} catch (const std::exception& e) {
			log_.warn("collect failed", {{"source", s->name()}, {"err", e}});
		}
	}
	if (b.empty()) {
		return;
	}
	try {
		c_.processors.process(b);
	} catch (const std::exception& e) {
		log_.warn("process failed", {{"batch", b.id}, {"err", e}});
	}
	etrace::v1::Batch pb;
	wire::to_proto(b, pb);
	std::string data;
	if (!pb.SerializeToString(&data)) {
		log_.error("encode batch", {{"batch", b.id}});
		return;
	}
	try {
		c_.queue->put(data);
	} catch (const std::exception& e) {
		log_.error("enqueue batch", {{"err", e}});
		return;
	}
	collected_.Increment();
	{
		std::lock_guard lock(wake_mu_);
		wake_ = true;
	}
	wake_cv_.notify_one();
}

void Pipeline::ship(std::stop_token st) {
	int attempt = 0;
	while (!st.stop_requested()) {
		auto rec = c_.queue->peek();
		if (!rec) {
			std::unique_lock lock(wake_mu_);
			if (!wake_cv_.wait(lock, st, [this] { return wake_; })) {
				return;
			}
			wake_ = false;
			continue;
		}
		try {
			deliver(st, *rec);
			c_.queue->ack(rec->id);
			exported_.Add({{"result", "ok"}}).Increment();
			attempt = 0;
		} catch (const PermanentError& e) {
			log_.error("dropping rejected batch", {{"record", rec->id}, {"err", e}});
			c_.queue->ack(rec->id);
			exported_.Add({{"result", "dropped"}}).Increment();
			attempt = 0;
		} catch (const std::exception& e) {
			if (st.stop_requested()) {
				return;
			}
			exported_.Add({{"result", "retry"}}).Increment();
			log_.warn("export failed, will retry", {{"record", rec->id}, {"attempt", attempt}, {"err", e}});
			if (!c_.retry.wait(st, attempt)) {
				return;
			}
			attempt++;
		}
	}
}

void Pipeline::deliver(std::stop_token st, const spool::Record& rec) {
	etrace::v1::Batch pb;
	if (!pb.ParseFromString(rec.data)) {
		throw PermanentError("decode queued batch: invalid protobuf");
	}
	model::Batch b;
	try {
		b = wire::from_proto(pb);
	} catch (const wire::InvalidBatch& e) {
		throw PermanentError(std::format("decode queued batch: {}", e.what()));
	}
	c_.exporter->export_batch(st, b);
}

void Pipeline::close() {
	std::string first;
	for (auto& s : c_.sources) {
		try {
			s->close();
		} catch (const std::exception& e) {
			if (first.empty()) {
				first = std::format("close source {}: {}", s->name(), e.what());
			}
		}
	}
	c_.processors = ProcessorChain();
	c_.exporter.reset();
	if (!first.empty()) {
		throw Error(first);
	}
}

}  // namespace etrace::agent
