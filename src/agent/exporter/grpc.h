#pragma once

#include <memory>

#include "agent/plugin.h"
#include "etrace/v1/ingest.grpc.pb.h"
#include "platform/grpc.h"

namespace etrace::agent::grpcexp {

struct Options : grpcx::ClientConfig {
	Duration timeout = 10s;

	void validate() const;
};

void decode_fields(config::Fields& f, Options& o);

// Exporter pushes batches to the collectors over gRPC. Batches larger than
// max_send_msg_bytes are sent in parts. InvalidArgument and authentication
// failures are permanent; everything else is retried.
class Exporter : public agent::Exporter {
public:
	explicit Exporter(Options o);
	void export_batch(std::stop_token st, const model::Batch& b) override;

private:
	void push(std::stop_token st, const etrace::v1::PushRequest& req);

	Options opts_;
	std::shared_ptr<grpc::Channel> channel_;
	std::unique_ptr<etrace::v1::IngestService::Stub> stub_;
};

}  // namespace etrace::agent::grpcexp
