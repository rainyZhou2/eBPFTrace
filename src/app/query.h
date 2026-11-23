#pragma once

#include <stop_token>

#include "app/common.h"
#include "platform/config.h"
#include "platform/http.h"

namespace etrace::app::query {

struct Config {
	Common common;
	http::ServerConfig http;
	Duration max_range = 7 * 24h;
	Duration trace_lookback = 24h;
	int max_chain_depth = 4;
	int max_events = 5000;
	config::Plugin storage;
	config::Plugin trace_backend;

	Config() { common.admin.listen = ":9433"; }
	void validate() const;
};

void decode_fields(config::Fields& f, Config& c);

void run(std::stop_token st, const Config& cfg);

}  // namespace etrace::app::query
