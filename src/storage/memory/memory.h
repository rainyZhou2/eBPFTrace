#pragma once

#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "platform/config.h"
#include "storage/storage.h"

namespace etrace::storage::memory {

struct Options {
	Duration retention = 1h;
};

void decode_fields(config::Fields& f, Options& o);

// Store keeps batches in process memory for single-node use and tests.
// Batches whose window ended before the retention are dropped on write.
class Store : public storage::Store {
public:
	explicit Store(Options o, std::function<Time()> clock = etrace::now);

	void write_batch(const model::Batch& b) override;
	std::vector<ChainValue> offcpu_chains(const ChainQuery& q) override;
	std::vector<Event> events(const EventQuery& q) override;
	void visit_events(const EventQuery& q, const EventVisitor& visit) override;
	void ping(Duration) override {}
	void close() override {}

private:
	void evict_locked();
	template <class Fn>
	void each_batch(const Filter& f, Fn&& fn);

	Options opts_;
	std::function<Time()> now_;
	std::shared_mutex mu_;
	std::vector<std::shared_ptr<const model::Batch>> batches_;
	std::unordered_set<std::string> seen_;
};

}  // namespace etrace::storage::memory
