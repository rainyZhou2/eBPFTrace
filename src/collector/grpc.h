#pragma once

#include <set>
#include <string>
#include <vector>

#include "collector/service.h"
#include "etrace/v1/ingest.grpc.pb.h"
#include "platform/log.h"

namespace etrace::collector {

// GrpcHandler adapts IngestService.Push to an Ingester. When tokens are
// configured every call must carry "authorization: Bearer <token>".
class GrpcHandler final : public etrace::v1::IngestService::Service {
public:
	GrpcHandler(Ingester& ing, const std::vector<std::string>& tokens, log::Logger log);

	grpc::Status Push(grpc::ServerContext* ctx, const etrace::v1::PushRequest* req,
	                  etrace::v1::PushResponse* resp) override;

private:
	bool authorized(const grpc::ServerContext& ctx) const;

	Ingester& ingester_;
	std::set<std::string> allowed_;
	log::Logger log_;
};

}  // namespace etrace::collector
