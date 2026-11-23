#include "app/collector.h"

#include "collector/grpc.h"
#include "platform/lifecycle.h"
#include "platform/sync.h"
#include "storage/drivers.h"

namespace etrace::app::collector {

void decode_fields(config::Fields& f, Config& c) {
	decode_common(f, c.common);
	f("grpc", c.grpc)("tokens", c.tokens)("dedup", c.dedup)("limits", c.limits)("storage", c.storage);
}

void Config::validate() const {
	if (storage.driver.empty()) {
		throw Error("storage.driver is required");
	}
}

void run(std::stop_token st, const Config& cfg) {
	auto log = log::make(cfg.common.log, "etrace-collector");
	telemetry::Registry reg;
	auto store = storage::open(cfg.storage);
	etrace::collector::Service svc(*store, cfg.dedup, cfg.limits, reg, log);
	etrace::collector::GrpcHandler handler(svc, cfg.tokens, log);

	grpcx::Server srv(cfg.grpc, log);
	srv.register_service(&handler);

	telemetry::Admin admin(cfg.common.admin, reg, log);
	admin.add_readiness_check("storage", [&](Duration timeout) { store->ping(timeout); });

	// The storage health watcher propagates storage health to the gRPC
	// health service so that agents' client-side balancers route around
	// replicas that cannot write.
	lifecycle::FuncComponent health("storage-health", [&](std::stop_token s) {
		do {
			bool ok = true;
			try {
				store->ping(2s);
			} catch (const std::exception&) {
				ok = false;
			}
			srv.set_serving(ok);
		} while (sleep_for(s, 5s));
	});
	lifecycle::FuncComponent admin_c("admin", [&](std::stop_token s) { admin.run(s); });
	lifecycle::FuncComponent grpc_c("grpc", [&](std::stop_token s) { srv.run(s); });

	log.info("collector starting", {{"storage", cfg.storage.driver}, {"version", version}});
	lifecycle::run(st, cfg.common.shutdown_timeout, {&admin_c, &grpc_c, &health}, {[&] { store->close(); }});
}

}  // namespace etrace::app::collector
