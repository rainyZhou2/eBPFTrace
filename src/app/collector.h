#pragma once

#include <stop_token>
#include <string>
#include <vector>

#include "app/common.h"
#include "collector/service.h"
#include "platform/config.h"
#include "platform/grpc.h"

namespace etrace::app::collector {

struct Config {
	Common common;
	grpcx::ServerConfig grpc;
	std::vector<std::string> tokens;
	etrace::collector::DedupConfig dedup;
	etrace::collector::Limits limits;
	config::Plugin storage;

	Config() { common.admin.listen = ":9432"; }
	void validate() const;
};

void decode_fields(config::Fields& f, Config& c);

void run(std::stop_token st, const Config& cfg);

}  // namespace etrace::app::collector
