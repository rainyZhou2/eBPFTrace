#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "platform/config.h"
#include "platform/error.h"
#include "platform/time.h"

namespace etrace::tracing {

class NotFound : public Error {
public:
	NotFound() : Error("trace not found") {}
};

struct Span {
	std::string span_id;
	std::string parent_id;
	std::string name;
	std::string service;
	Time start{};
	Time end{};
	std::map<std::string, std::string> attrs;

	Duration duration() const { return end - start; }
};

// Backend fetches spans of a trace from an external tracing system so that
// off-CPU intervals can be placed on the request's span tree.
class Backend {
public:
	virtual ~Backend() = default;
	// trace throws NotFound for unknown traces and Error otherwise.
	virtual std::vector<Span> trace(const std::string& trace_id) = 0;
};

// open returns nullptr when no driver is configured; "jaeger" is the only
// driver.
std::unique_ptr<Backend> open(const config::Plugin& cfg);

}  // namespace etrace::tracing
