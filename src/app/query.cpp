#include "app/query.h"

#include "platform/lifecycle.h"
#include "query/http.h"
#include "query/service.h"
#include "query/ui.h"
#include "storage/drivers.h"
#include "tracing/tracing.h"

namespace etrace::app::query {

void decode_fields(config::Fields& f, Config& c) {
	decode_common(f, c.common);
	f("http", c.http)("max_range", c.max_range)("trace_lookback", c.trace_lookback)(
	        "max_chain_depth", c.max_chain_depth)("max_events", c.max_events)("storage", c.storage)(
	        "trace_backend", c.trace_backend);
}

void Config::validate() const {
	if (storage.driver.empty()) {
		throw Error("storage.driver is required");
	}
}

void run(std::stop_token st, const Config& cfg) {
	auto log = log::make(cfg.common.log, "etrace-query");
	telemetry::Registry reg;
	auto store = storage::open(cfg.storage);
	auto backend = tracing::open(cfg.trace_backend);

	etrace::query::Service svc(*store);
	etrace::query::Handler handler(svc, backend.get(),
	                               etrace::query::HandlerConfig{cfg.max_range, 15min, cfg.trace_lookback,
	                                                            cfg.max_chain_depth, cfg.max_events},
	                               log);
	http::Server srv("query-http", cfg.http, log);
	handler.routes(srv.server());
	etrace::query::ui::routes(srv.server());

	telemetry::Admin admin(cfg.common.admin, reg, log);
	admin.add_readiness_check("storage", [&](Duration timeout) { store->ping(timeout); });

	log.info("query starting", {{"storage", cfg.storage.driver}, {"version", version}});
	lifecycle::FuncComponent admin_c("admin", [&](std::stop_token s) { admin.run(s); });
	lifecycle::FuncComponent http_c("query-http", [&](std::stop_token s) { srv.run(s); });
	lifecycle::run(st, cfg.common.shutdown_timeout, {&admin_c, &http_c}, {[&] { store->close(); }});
}

}  // namespace etrace::app::query
