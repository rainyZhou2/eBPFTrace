#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "platform/config.h"
#include "platform/log.h"
#include "platform/lru.h"
#include "platform/telemetry.h"
#include "symbol/resolver.h"

namespace etrace::symbol {

struct DebuginfodConfig {
	std::vector<std::string> urls;
	std::string cache_dir = "/var/lib/etrace/debuginfod";
	int64_t cache_max_bytes = int64_t{2} << 30;
	int64_t max_file_bytes = int64_t{512} << 20;
	Duration timeout = 2min;
	int workers = 2;
	int queue_size = 256;
	Duration retry_after = 10min;

	void validate() const;
};

void decode_fields(config::Fields& f, DebuginfodConfig& c);

// Debuginfod downloads debug files from debuginfod servers into a bounded
// disk cache. find never blocks on the network: a missing file is queued
// for the background workers and reported as not found until it arrives,
// so stacks of a freshly seen stripped binary are symbolized with exported
// symbols only for the first intervals.
class Debuginfod : public DebugFinder {
public:
	Debuginfod(DebuginfodConfig cfg, log::Logger log, telemetry::Registry& reg);
	~Debuginfod() override;

	std::optional<std::string> find(const std::string& build_id) override;

private:
	std::string path(const std::string& id) const;
	void request(const std::string& id);
	void work(std::stop_token st);
	std::string fetch(std::stop_token st, const std::string& id);
	std::string fetch_from(std::stop_token st, const std::string& url, const std::string& id);
	void prune();

	DebuginfodConfig cfg_;
	log::Logger log_;
	std::mutex mu_;
	std::condition_variable_any cv_;
	std::deque<std::string> queue_;
	std::set<std::string> inflight_;
	LruCache<std::string, bool> failed_;
	std::mutex prune_mu_;
	prometheus::Family<prometheus::Counter>& fetches_;
	prometheus::Counter& dropped_;
	std::vector<std::jthread> workers_;
};

// valid_build_id accepts lower-case hex IDs of a plausible length.
bool valid_build_id(const std::string& id);

}  // namespace etrace::symbol
