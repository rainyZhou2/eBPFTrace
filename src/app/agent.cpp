#include "app/agent.h"

#include <openssl/evp.h>
#include <unistd.h>

#include <format>

#include "agent/pipeline.h"
#include "agent/plugin.h"
#include "platform/hex.h"
#include "platform/lifecycle.h"
#include "platform/spool.h"
#include "platform/telemetry.h"

namespace etrace::app::agent {

namespace {

std::string agent_id(const std::string& node) {
	unsigned char sum[EVP_MAX_MD_SIZE];
	unsigned int n = 0;
	if (EVP_Digest(node.data(), node.size(), sum, &n, EVP_sha256(), nullptr) != 1 || n < 8) {
		throw Error("agent id: sha256 failed");
	}
	return hex_encode(std::span<const uint8_t>(sum, 8));
}

}  // namespace

void decode_fields(config::Fields& f, SpoolConfig& c) {
	f("dir", c.dir)("max_bytes", c.max_bytes);
}

void decode_fields(config::Fields& f, Config& c) {
	decode_common(f, c.common);
	f("node", c.node)("labels", c.labels)("proc_root", c.proc_root)("interval", c.interval)("spool", c.spool)(
	        "retry", c.retry)("sources", c.sources)("processors", c.processors)("exporter", c.exporter);
}

void Config::validate() {
	if (node.empty()) {
		char host[256] = {};
		if (gethostname(host, sizeof host - 1) != 0 || host[0] == '\0') {
			throw Error("node is required");
		}
		node = host;
	}
	if (interval < 1s) {
		throw Error("interval must be at least 1s");
	}
	if (sources.empty()) {
		throw Error("at least one source is required");
	}
	if (exporter.driver.empty()) {
		throw Error("exporter.driver is required");
	}
	if (spool.dir.empty()) {
		throw Error("spool.dir is required");
	}
}

void run(std::stop_token st, const Config& cfg) {
	auto log = log::make(cfg.common.log, "etrace-agent");
	telemetry::Registry reg;
	telemetry::Admin admin(cfg.common.admin, reg, log);
	spool::Spool queue(cfg.spool.dir, cfg.spool.max_bytes);

	etrace::agent::Deps deps{log, reg, cfg.proc_root};
	etrace::agent::PipelineConfig pc;
	pc.resource = model::Resource{cfg.node, agent_id(cfg.node), version, cfg.labels};
	pc.interval = cfg.interval;
	for (const auto& sc : cfg.sources) {
		pc.sources.push_back(etrace::agent::new_source(sc, deps));
	}
	for (const auto& p : cfg.processors) {
		pc.processors.add(etrace::agent::new_processor(p, deps));
	}
	pc.exporter = etrace::agent::new_exporter(cfg.exporter, deps);
	pc.queue = &queue;
	pc.retry = cfg.retry;
	pc.log = log;
	etrace::agent::Pipeline pipe(std::move(pc), reg);

	auto max_bytes = cfg.spool.max_bytes;
	admin.add_readiness_check("spool", [&queue, max_bytes](Duration) {
		if (max_bytes > 0 && queue.bytes() >= max_bytes) {
			throw Error("spool full");
		}
	});

	log.info("agent starting", {{"node", cfg.node}, {"version", version}});
	lifecycle::FuncComponent admin_c("admin", [&](std::stop_token s) { admin.run(s); });
	lifecycle::FuncComponent pipe_c("pipeline", [&](std::stop_token s) { pipe.run(s); });
	lifecycle::run(st, cfg.common.shutdown_timeout, {&admin_c, &pipe_c}, {[&] { pipe.close(); }});
}

}  // namespace etrace::app::agent
