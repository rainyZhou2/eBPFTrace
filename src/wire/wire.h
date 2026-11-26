#pragma once

#include <cstddef>

#include "etrace/v1/ingest.pb.h"
#include "model/model.h"
#include "platform/error.h"

namespace etrace::wire {

// Limits bounds the in-memory size a decoded batch may reach. A zero value
// disables that limit.
struct Limits {
	size_t max_samples = 200000;
	size_t max_events = 100000;
	size_t max_frames = 256;
	size_t max_hops = 8;
	size_t max_string_bytes = 64 * 1024;
	size_t max_labels = 256;
};

// InvalidBatch reports a protobuf batch that violates the wire contract.
class InvalidBatch : public Error {
public:
	using Error::Error;
};

void to_proto(const model::Batch& b, etrace::v1::Batch& out);
// from_proto validates the batch and throws InvalidBatch on violations.
model::Batch from_proto(const etrace::v1::Batch& p, const Limits& limits = {});

}  // namespace etrace::wire
