#pragma once

#include <string>

#include "platform/config.h"
#include "platform/log.h"
#include "platform/telemetry.h"

namespace etrace::app {

// version is set at build time from the VCS description.
extern const char* const version;

struct Common {
	log::Config log;
	telemetry::AdminConfig admin;
	Duration shutdown_timeout = 20s;
};

// decode_common reads the shared top-level keys of every service config.
void decode_common(config::Fields& f, Common& c);

}  // namespace etrace::app
