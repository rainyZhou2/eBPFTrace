#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "agent/plugin.h"
#include "model/model.h"
#include "platform/backoff.h"
#include "platform/log.h"
#include "platform/spool.h"
#include "platform/telemetry.h"

namespace etrace::agent {

struct PipelineConfig {
	model::Resource resource;
	Duration interval = 10s;
	std::vector<std::unique_ptr<Source>> sources;
	ProcessorChain processors;
	spool::Spool* queue = nullptr;
	std::unique_ptr<Exporter> exporter;
	backoff::Policy retry;
	log::Logger log = log::Logger::discard();
};

// Pipeline decouples collection from delivery through a durable queue:
// sources are drained on a fixed interval, processed, persisted, and a
// separate shipper thread delivers queued batches with retry.
class Pipeline {
public:
	Pipeline(PipelineConfig c, telemetry::Registry& reg);
	~Pipeline();

	std::string name() const { return "pipeline"; }
	void run(std::stop_token st);
	// close stops the sources and releases processors and the exporter.
	void close();

private:
	void collect(Time start, Time end);
	void ship(std::stop_token st);
	void deliver(std::stop_token st, const spool::Record& rec);

	PipelineConfig c_;
	log::Logger log_;
	prometheus::Counter& collected_;
	prometheus::Family<prometheus::Counter>& exported_;
	std::mutex wake_mu_;
	std::condition_variable_any wake_cv_;
	bool wake_ = false;
};

}  // namespace etrace::agent
