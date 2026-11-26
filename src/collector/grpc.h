#pragma once

#include <string>

#include "collector/auth.h"
#include "collector/service.h"
#include "etrace/v1/ingest.grpc.pb.h"
#include "platform/log.h"
#include "wire/wire.h"

namespace etrace::collector {

// GrpcHandler adapts IngestService.Push to an Ingester. The authenticator
// runs before the payload is decoded.
class GrpcHandler final : public etrace::v1::IngestService::Service {
public:
	GrpcHandler(Ingester& ing, Auth auth, wire::Limits limits, log::Logger log);

	grpc::Status Push(grpc::ServerContext* ctx, const etrace::v1::PushRequest* req,
	                  etrace::v1::PushResponse* resp) override;

private:
	bool authorized(const grpc::ServerContext& ctx) const;

	Ingester& ingester_;
	Auth auth_;
	wire::Limits limits_;
	log::Logger log_;
};

}  // namespace etrace::collector
