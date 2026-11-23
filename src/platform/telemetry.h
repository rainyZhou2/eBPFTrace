#pragma once

#include <prometheus/collectable.h>
#include <prometheus/counter.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

#include "platform/config.h"
#include "platform/log.h"
#include "platform/time.h"

namespace etrace::telemetry {

// Registry holds the metrics of one process: regular prometheus-cpp
// families plus values computed at scrape time.
class Registry {
public:
	explicit Registry(bool process_metrics = true);

	prometheus::Registry& prom() { return *prom_; }

	prometheus::Counter& counter(const std::string& name, const std::string& help = "",
	                             const std::map<std::string, std::string>& labels = {});
	prometheus::Family<prometheus::Counter>& counter_family(const std::string& name, const std::string& help = "");
	prometheus::Gauge& gauge(const std::string& name, const std::string& help = "",
	                         const std::map<std::string, std::string>& labels = {});
	prometheus::Family<prometheus::Gauge>& gauge_family(const std::string& name, const std::string& help = "");
	prometheus::Histogram& histogram(const std::string& name, const std::vector<double>& buckets,
	                                 const std::string& help = "");
	void gauge_func(const std::string& name, const std::string& help, std::function<double()> fn);
	void counter_func(const std::string& name, const std::string& help, std::function<double()> fn);

	std::vector<prometheus::MetricFamily> gather() const;
	std::string text() const;

private:
	struct Func {
		std::string name;
		std::string help;
		prometheus::MetricType type;
		std::function<double()> fn;
	};
	std::shared_ptr<prometheus::Registry> prom_;
	bool process_;
	mutable std::mutex mu_;
	std::vector<Func> funcs_;
};

std::vector<double> exponential_buckets(double start, double factor, int count);

struct AdminConfig {
	std::string listen;
	Duration check_timeout = 2s;
};

void decode_fields(config::Fields& f, AdminConfig& c);

// A readiness check throws to report the component as not ready. It gets
// the time it may take.
using Check = std::function<void(Duration timeout)>;

// Admin serves /metrics, /healthz and /readyz.
class Admin {
public:
	Admin(AdminConfig cfg, const Registry& reg, log::Logger log);
	~Admin();

	void add_readiness_check(const std::string& name, Check c);
	std::string name() const { return "admin"; }
	void run(std::stop_token st);

	// ready runs every check and returns the HTTP status and JSON body.
	std::pair<int, std::string> ready();

private:
	AdminConfig cfg_;
	const Registry& reg_;
	log::Logger log_;
	std::mutex mu_;
	std::map<std::string, Check> checks_;
};

}  // namespace etrace::telemetry
