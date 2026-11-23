#include "app/common.h"

#ifndef ETRACE_VERSION
#define ETRACE_VERSION "dev"
#endif

namespace etrace::log {
inline void decode_fields(config::Fields& f, Config& c) {
	f("level", c.level)("format", c.format);
}
}  // namespace etrace::log

namespace etrace::app {

const char* const version = ETRACE_VERSION;

void decode_common(config::Fields& f, Common& c) {
	f("log", c.log)("admin", c.admin)("shutdown_timeout", c.shutdown_timeout);
}

}  // namespace etrace::app
