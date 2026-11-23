#pragma once

#include <string>
#include <vector>

#include "platform/config.h"
#include "platform/time.h"
#include "platform/url.h"
#include "tracing/tracing.h"

namespace etrace::tracing::jaeger {

struct Options {
	std::string url;
	Duration timeout = 5s;
	std::string token;

	void validate() const;
};

void decode_fields(config::Fields& f, Options& o);

// Backend reads traces through the Jaeger query HTTP API, which Jaeger and
// Grafana Tempo (jaeger-query compatible mode) both serve.
class Backend : public tracing::Backend {
public:
	explicit Backend(Options o);
	std::vector<Span> trace(const std::string& trace_id) override;

private:
	Options opts_;
	HttpUrl base_;
};

}  // namespace etrace::tracing::jaeger
