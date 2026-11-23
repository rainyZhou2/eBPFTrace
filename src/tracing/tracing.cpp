#include <format>

#include "tracing/jaeger.h"
#include "tracing/tracing.h"

namespace etrace::tracing {

std::unique_ptr<Backend> open(const config::Plugin& cfg) {
	if (cfg.driver.empty()) {
		return nullptr;
	}
	if (cfg.driver == "jaeger") {
		return std::make_unique<jaeger::Backend>(cfg.options_as<jaeger::Options>());
	}
	throw Error(std::format("unknown trace backend \"{}\", available: [jaeger]", cfg.driver));
}

}  // namespace etrace::tracing
