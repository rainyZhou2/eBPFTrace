#pragma once

#include <httplib.h>

#include <functional>

#include "platform/log.h"
#include "platform/time.h"
#include "query/service.h"
#include "tracing/tracing.h"

namespace etrace::query {

struct HandlerConfig {
	Duration max_range{0};
	Duration default_span{0};
	Duration trace_lookback{0};
	int max_chain_depth = 0;
	int max_event_result = 0;
};

// Handler maps the HTTP API onto Service: parameter parsing and validation,
// status codes and JSON rendering.
class Handler {
public:
	Handler(Service& svc, tracing::Backend* backend, HandlerConfig cfg, log::Logger log,
	        std::function<Time()> clock = etrace::now);

	void routes(httplib::Server& srv);

	// The handlers are public so tests can call them without a socket.
	void flamegraph(const httplib::Request& req, httplib::Response& res);
	void causes(const httplib::Request& req, httplib::Response& res);
	void events(const httplib::Request& req, httplib::Response& res);
	void trace(const httplib::Request& req, httplib::Response& res, const std::string& trace_id);

private:
	storage::Filter parse_filter(const httplib::Request& req, Duration span) const;
	int depth(const httplib::Request& req) const;
	int event_limit(const httplib::Request& req) const;
	void internal(httplib::Response& res, const std::exception& e) const;

	Service& svc_;
	tracing::Backend* backend_;
	HandlerConfig cfg_;
	log::Logger log_;
	std::function<Time()> now_;
};

}  // namespace etrace::query
