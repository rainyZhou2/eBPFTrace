#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "model/model.h"
#include "platform/config.h"
#include "platform/error.h"
#include "platform/time.h"

namespace etrace::storage {

// DuplicateError reports a batch ID that was stored before.
class DuplicateError : public Error {
public:
	DuplicateError() : Error("batch already stored") {}
};

// Writer persists batches. Implementations must be idempotent on Batch.id
// so that agents can retry safely; a repeated ID throws DuplicateError or
// succeeds without storing twice.
class Writer {
public:
	virtual ~Writer() = default;
	virtual void write_batch(const model::Batch& b) = 0;
};

struct Filter {
	Time from{};
	Time to{};
	std::string node;
	std::string comm;
	uint32_t pid = 0;
	std::string container_id;
	std::string ns;
	std::string pod;
	std::map<std::string, std::string> labels;

	bool match_task(const model::Task& t) const;
};

struct Hop {
	std::string comm;
	uint32_t pid = 0;
	std::vector<std::string> wake_stack;
	std::vector<std::string> wait_stack;
	uint64_t wait_ns = 0;
	std::string trace_id;
	std::string span_id;
};

// ChainValue aggregates off-CPU time by blocked task, its stack, and the
// causal chain truncated to the requested depth.
struct ChainValue {
	std::string comm;
	std::vector<std::string> stack;
	std::vector<Hop> chain;
	uint64_t total_ns = 0;
	uint64_t count = 0;
};

struct ChainQuery {
	Filter filter;
	int depth = 0;
	int limit = 0;
};

struct Event {
	std::string node;
	uint32_t pid = 0;
	uint32_t tid = 0;
	std::string comm;
	std::string container_id;
	std::string ns;
	std::string pod;
	std::string container;
	std::vector<std::string> stack;
	std::vector<Hop> chain;
	Time start{};
	Time end{};
	std::string reason;
	std::string trace_id;
	std::string span_id;
	// GPU waits only, see model::GPUWait.
	uint64_t gpu_stream = 0;
	uint32_t gpu_launches = 0;
};

struct EventQuery {
	Filter filter;
	std::string trace_id;
	std::string span_id;
	Duration min_duration{0};
	int limit = 0;
};

using EventVisitor = std::function<void(Event)>;

class Reader {
public:
	virtual ~Reader() = default;
	virtual std::vector<ChainValue> offcpu_chains(const ChainQuery& q) = 0;
	virtual std::vector<Event> events(const EventQuery& q) = 0;
	// Visit matching events in start-time order. A zero limit visits all matches.
	virtual void visit_events(const EventQuery& q, const EventVisitor& visit) = 0;
};

class Store : public Writer, public Reader {
public:
	// ping throws when the store cannot serve requests within timeout.
	virtual void ping(Duration timeout) = 0;
	virtual void close() = 0;
};

std::vector<Hop> hops(const std::vector<model::Hop>& c);
std::vector<Hop> truncate(std::vector<Hop> c, int depth);

template <class T>
std::vector<T> limit(std::vector<T> s, int n) {
	if (n > 0 && s.size() > static_cast<size_t>(n)) {
		s.resize(static_cast<size_t>(n));
	}
	return s;
}

}  // namespace etrace::storage
