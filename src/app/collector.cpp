#include "app/collector.h"

#include <utility>

#include "collector/grpc.h"
#include "platform/lifecycle.h"
#include "platform/sync.h"
#include "storage/drivers.h"
#include "wire/wire.h"

namespace etrace::app::collector {

void decode_fields(config::Fields& f, Config& c) {
	decode_common(f, c.common);
	f("grpc", c.grpc)("tokens", c.tokens)("allow_unauthenticated", c.allow_unauthenticated)("dedup", c.dedup)(
	        "limits", c.limits)("storage", c.storage);
}

etrace::collector::AuthConfig Config::auth_config() const {
	return etrace::collector::AuthConfig{tokens, allow_unauthenticated,
	                                     grpc.tls.enabled && grpc.tls.require_client_cert};
}

void Config::validate() const {
	if (storage.driver.empty()) {
		throw Error("storage.driver is required");
	}
	grpc.validate();
	auth_config().validate();
}

void run(std::stop_token st, const Config& cfg) {
	auto log = log::make(cfg.common.log, "etrace-collector");
	telemetry::Registry reg;
	auto store = storage::open(cfg.storage);
	etrace::collector::Service svc(*store, cfg.dedup, cfg.limits, reg, log);
	auto auth_config = cfg.auth_config();
	auto auth = etrace::collector::Auth(auth_config);
	if (auth_config.allow_unauthenticated) {
		log.warn("collector authentication is disabled");
	} else if (auth_config.allow_mtls) {
		log.info("collector authentication uses client certificates");
	}
	auto to_limit = [](int n) { return n > 0 ? static_cast<size_t>(n) : size_t{0}; };
	wire::Limits wire_limits{to_limit(cfg.limits.max_samples_per_batch), to_limit(cfg.limits.max_events_per_batch),
	                         to_limit(cfg.limits.max_frames_per_stack), to_limit(cfg.limits.max_chain_hops)};
	etrace::collector::GrpcHandler handler(svc, std::move(auth), wire_limits, log);

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
