#include "collector/grpc.h"

#include <string_view>
#include <utility>

#include "wire/wire.h"

namespace etrace::collector {

GrpcHandler::GrpcHandler(Ingester& ing, Auth auth, wire::Limits limits, log::Logger log)
    : ingester_(ing), auth_(std::move(auth)), limits_(limits), log_(std::move(log)) {}

bool GrpcHandler::authorized(const grpc::ServerContext& ctx) const {
	if (auth_.anonymous()) {
		return true;
	}
	auto range = ctx.client_metadata().equal_range("authorization");
	for (auto it = range.first; it != range.second; ++it) {
		if (auth_.authorize(std::string_view(it->second.data(), it->second.size()))) {
			return true;
		}
	}
	return false;
}

grpc::Status GrpcHandler::Push(grpc::ServerContext* ctx, const etrace::v1::PushRequest* req,
                               etrace::v1::PushResponse* resp) {
	if (!authorized(*ctx)) {
		return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "invalid token");
	}
	try {
		auto b = wire::from_proto(req->batch(), limits_);
		if (ctx->IsCancelled()) {
			return grpc::Status(grpc::StatusCode::CANCELLED, "context canceled");
		}
		resp->set_duplicate(ingester_.ingest(b));
		return grpc::Status::OK;
	} catch (const wire::InvalidBatch& e) {
		return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, e.what());
	} catch (const InvalidBatch& e) {
		return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, e.what());
	} catch (const std::exception&) {
		return grpc::Status(grpc::StatusCode::UNAVAILABLE, "storage unavailable");
	} catch (...) {
		log_.error("grpc handler failed with a non-standard exception", {{"method", "Push"}});
		return grpc::Status(grpc::StatusCode::INTERNAL, "internal error");
	}
}

}  // namespace etrace::collector
