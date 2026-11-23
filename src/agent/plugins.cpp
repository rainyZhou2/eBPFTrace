#include <format>

#include "agent/exporter/grpc.h"
#include "agent/exporter/pyroscope.h"
#include "agent/plugin.h"
#include "agent/processor/container.h"
#include "agent/processor/symbolize.h"
#include "agent/source/offcpu/source.h"

namespace etrace::agent {

std::unique_ptr<Source> new_source(const config::Plugin& cfg, Deps deps) {
	if (cfg.driver == "offcpu") {
		return std::make_unique<offcpu::Source>(cfg.options_as<offcpu::Options>(), deps);
	}
	throw Error(std::format("unknown source \"{}\", available: [offcpu]", cfg.driver));
}

std::unique_ptr<Processor> new_processor(const config::Plugin& cfg, Deps deps) {
	if (cfg.driver == "container") {
		return std::make_unique<container::Processor>(deps.proc_root, cfg.options_as<container::Options>());
	}
	if (cfg.driver == "symbolize") {
		return symbolize::Processor::from_options(cfg.options_as<symbolize::Options>(), deps);
	}
	throw Error(std::format("unknown processor \"{}\", available: [container symbolize]", cfg.driver));
}

std::unique_ptr<Exporter> new_exporter(const config::Plugin& cfg, Deps) {
	if (cfg.driver == "grpc") {
		return std::make_unique<grpcexp::Exporter>(cfg.options_as<grpcexp::Options>());
	}
	if (cfg.driver == "pyroscope") {
		return std::make_unique<pyroscope::Exporter>(cfg.options_as<pyroscope::Options>());
	}
	throw Error(std::format("unknown exporter \"{}\", available: [grpc pyroscope]", cfg.driver));
}

}  // namespace etrace::agent
