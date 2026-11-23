#pragma once

#include "etrace/v1/ingest.pb.h"
#include "model/model.h"
#include "platform/error.h"

namespace etrace::wire {

// InvalidBatch reports a protobuf batch that violates the wire contract.
class InvalidBatch : public Error {
public:
	using Error::Error;
};

void to_proto(const model::Batch& b, etrace::v1::Batch& out);
// from_proto validates the batch and throws InvalidBatch on violations.
model::Batch from_proto(const etrace::v1::Batch& p);

}  // namespace etrace::wire
