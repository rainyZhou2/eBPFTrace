#include "collector/grpc.h"

#include "wire/wire.h"

namespace etrace::collector {

GrpcHandler::GrpcHandler(Ingester& ing, const std::vector<std::string>& tokens, log::Logger log)
    : ingester_(ing), log_(std::move(log)) {
	for (const auto& t : tokens) {
		if (!t.empty()) {
			allowed_.insert("Bearer " + t);
		}
	}
}

bool GrpcHandler::authorized(const grpc::ServerContext& ctx) const {
	if (allowed_.empty()) {
		return true;
	}
	auto range = ctx.client_metadata().equal_range("authorization");
	for (auto it = range.first; it != range.second; ++it) {
		if (allowed_.contains(std::string(it->second.data(), it->second.size()))) {
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
		auto b = wire::from_proto(req->batch());
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
