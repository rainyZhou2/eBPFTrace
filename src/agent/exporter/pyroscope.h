#pragma once

#include <string>
#include <utility>
#include <vector>

#include "agent/plugin.h"
#include "platform/url.h"

namespace etrace::agent::pyroscope {

struct Options {
	std::string url;
	std::string service_name = "etrace";
	std::string tenant_id;
	std::string username;
	std::string password;
	Duration timeout = 10s;

	void validate() const;
};

void decode_fields(config::Fields& f, Options& o);

// Exporter sends off-CPU samples to Grafana Pyroscope as pprof profiles, so
// the agent can be used without the eTrace collector. It uses the push API
// (push.v1.PusherService/Push) in Connect's JSON form.
class Exporter : public agent::Exporter {
public:
	explicit Exporter(Options o);
	// export_batch sends one series per task identity in a single request.
	// The push is not idempotent: if the request fails after Pyroscope
	// stored it, the pipeline's retry stores the window twice.
	void export_batch(std::stop_token st, const model::Batch& b) override;

	// build_body returns the JSON request, or "" when there is nothing to
	// send.
	std::string build_body(const model::Batch& b) const;

private:
	std::vector<std::pair<std::string, std::string>> labels(const model::Resource& r, const model::Task& t,
	                                                        bool gpu) const;

	Options opts_;
	HttpUrl base_;
};

}  // namespace etrace::agent::pyroscope
