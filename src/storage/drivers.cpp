#include "storage/drivers.h"

#include <format>

#include "storage/clickhouse/clickhouse.h"
#include "storage/memory/memory.h"

namespace etrace::storage {

std::unique_ptr<Store> open(const config::Plugin& cfg) {
	if (cfg.driver == "memory") {
		return std::make_unique<memory::Store>(cfg.options_as<memory::Options>());
	}
	if (cfg.driver == "clickhouse") {
		return std::make_unique<clickhouse::Store>(cfg.options_as<clickhouse::Options>());
	}
	throw Error(std::format("unknown storage driver \"{}\", available: [clickhouse memory]", cfg.driver));
}

}  // namespace etrace::storage
