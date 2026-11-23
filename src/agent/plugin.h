#pragma once

#include <memory>
#include <stop_token>
#include <string>
#include <vector>

#include "model/model.h"
#include "platform/config.h"
#include "platform/error.h"
#include "platform/log.h"
#include "platform/telemetry.h"

namespace etrace::agent {

struct Deps {
	log::Logger log;
	telemetry::Registry& registry;
	std::string proc_root;
};

// Source is a pluggable signal producer. collect appends everything gathered
// since the previous call into the batch.
class Source {
public:
	virtual ~Source() = default;
	virtual std::string name() const = 0;
	virtual void start() = 0;
	virtual void collect(model::Batch& b) = 0;
	// close detaches probes and stops background threads; it is safe to
	// call more than once.
	virtual void close() = 0;
};

class Processor {
public:
	virtual ~Processor() = default;
	virtual std::string name() const = 0;
	virtual void process(model::Batch& b) = 0;
};

// ProcessorChain runs processors in order; destroying it releases the
// processors that own background work.
class ProcessorChain {
public:
	void add(std::unique_ptr<Processor> p) { procs_.push_back(std::move(p)); }
	void process(model::Batch& b) {
		for (auto& p : procs_) {
			p->process(b);
		}
	}
	size_t size() const { return procs_.size(); }

private:
	std::vector<std::unique_ptr<Processor>> procs_;
};

// PermanentError marks a batch that will never be accepted and must be
// dropped instead of retried.
class PermanentError : public Error {
public:
	using Error::Error;
};

class Exporter {
public:
	virtual ~Exporter() = default;
	// export_batch throws PermanentError for rejected batches and any other
	// exception for failures worth retrying. Stopping st cancels the call.
	virtual void export_batch(std::stop_token st, const model::Batch& b) = 0;
};

// The factories build the configured driver or throw Error naming the
// available ones.
std::unique_ptr<Source> new_source(const config::Plugin& cfg, Deps deps);
std::unique_ptr<Processor> new_processor(const config::Plugin& cfg, Deps deps);
std::unique_ptr<Exporter> new_exporter(const config::Plugin& cfg, Deps deps);

}  // namespace etrace::agent
