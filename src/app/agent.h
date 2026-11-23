#pragma once

#include <map>
#include <stop_token>
#include <string>
#include <vector>

#include "app/common.h"
#include "platform/backoff.h"
#include "platform/config.h"

namespace etrace::app::agent {

struct SpoolConfig {
	std::string dir = "/var/lib/etrace/spool";
	int64_t max_bytes = int64_t{512} << 20;
};

struct Config {
	Common common;
	std::string node;
	std::map<std::string, std::string> labels;
	std::string proc_root = "/proc";
	Duration interval = 10s;
	SpoolConfig spool;
	backoff::Policy retry;
	std::vector<config::Plugin> sources;
	std::vector<config::Plugin> processors;
	config::Plugin exporter;

	Config() { common.admin.listen = ":9431"; }
	// validate fills node from the hostname when unset.
	void validate();
};

void decode_fields(config::Fields& f, SpoolConfig& c);
void decode_fields(config::Fields& f, Config& c);

void run(std::stop_token st, const Config& cfg);

}  // namespace etrace::app::agent
