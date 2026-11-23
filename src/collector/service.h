#pragma once

#include <string>

#include "model/model.h"
#include "platform/config.h"
#include "platform/error.h"
#include "platform/log.h"
#include "platform/lru.h"
#include "platform/telemetry.h"
#include "storage/storage.h"

namespace etrace::collector {

// InvalidBatch rejects a batch that exceeds the configured limits; retrying
// it cannot succeed.
class InvalidBatch : public Error {
public:
	explicit InvalidBatch(const std::string& why) : Error("invalid batch: " + why) {}
};

class Ingester {
public:
	virtual ~Ingester() = default;
	// ingest returns true when the batch was a duplicate of an earlier one.
	virtual bool ingest(const model::Batch& b) = 0;
};

struct DedupConfig {
	int size = 100000;
	Duration ttl = 1h;
};

void decode_fields(config::Fields& f, DedupConfig& c);

struct Limits {
	int max_samples_per_batch = 200000;
	int max_events_per_batch = 100000;
	int max_frames_per_stack = 256;
	int max_chain_hops = 8;
};

void decode_fields(config::Fields& f, Limits& l);

// Service validates and stores batches. It is safe to call repeatedly with
// the same batch: a local cache rejects fast retries and the storage layer
// deduplicates across collector replicas.
class Service : public Ingester {
public:
	Service(storage::Writer& w, DedupConfig dedup, Limits limits, telemetry::Registry& reg, log::Logger log);

	bool ingest(const model::Batch& b) override;

private:
	void validate(const model::Batch& b) const;
	void check_stacks(const model::Stack& st, const std::vector<model::Hop>& chain) const;

	storage::Writer& writer_;
	log::Logger log_;
	LruCache<std::string, bool> seen_;
	Limits limits_;
	prometheus::Family<prometheus::Counter>& batches_;
	prometheus::Counter& samples_;
	prometheus::Histogram& latency_;
};

}  // namespace etrace::collector
