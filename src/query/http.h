#pragma once

#include <httplib.h>

#include <functional>
#include <string>

#include "platform/log.h"
#include "platform/ratelimit.h"
#include "platform/time.h"
#include "query/auth.h"
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

struct HandlerOptions {
	HandlerConfig limits;
	AuthConfig auth;
	ratelimit::Policy rate_limit;
	bool trust_forwarded_for = false;
};

// Handler maps the HTTP API onto Service: parameter parsing and validation,
// status codes and JSON rendering. API routes are rate limited and require
// the configured credentials; UI assets stay public.
class Handler {
public:
	Handler(Service& svc, tracing::Backend* backend, HandlerOptions opts, log::Logger log,
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
	void internal(const httplib::Request& req, httplib::Response& res, const std::exception& e) const;

	bool check_auth(const httplib::Request& req, httplib::Response& res) const;
	std::string client_key(const httplib::Request& req) const;

	Service& svc_;
	tracing::Backend* backend_;
	HandlerConfig cfg_;
	Auth auth_;
	mutable ratelimit::Limiter limiter_;
	bool trust_forwarded_for_ = false;
	log::Logger log_;
	std::function<Time()> now_;
};

}  // namespace etrace::query
