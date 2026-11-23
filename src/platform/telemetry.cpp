#include "platform/telemetry.h"

#include <dirent.h>
#include <sys/resource.h>
#include <unistd.h>

#include <prometheus/text_serializer.h>

#include <cmath>
#include <fstream>
#include <future>
#include <nlohmann/json.hpp>
#include <sstream>

#include "platform/http.h"

namespace etrace::telemetry {

namespace {

prometheus::MetricFamily single(const std::string& name, const std::string& help, prometheus::MetricType type,
                                double v) {
	prometheus::MetricFamily mf;
	mf.name = name;
	mf.help = help;
	mf.type = type;
	prometheus::ClientMetric m;
	if (type == prometheus::MetricType::Counter) {
		m.counter.value = v;
	} else {
		m.gauge.value = v;
	}
	mf.metric.push_back(std::move(m));
	return mf;
}

// process_families reports the standard process_* metrics from procfs.
void process_families(std::vector<prometheus::MetricFamily>& out) {
	std::ifstream in("/proc/self/stat");
	std::string stat((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	auto close = stat.rfind(')');
	if (close == std::string::npos) {
		return;
	}
	std::istringstream fields(stat.substr(close + 2));
	std::vector<std::string> f;
	for (std::string tok; fields >> tok;) {
		f.push_back(tok);
	}
	// Fields after the command name start at field 3 (state).
	if (f.size() < 22) {
		return;
	}
	double ticks = static_cast<double>(sysconf(_SC_CLK_TCK));
	double page = static_cast<double>(sysconf(_SC_PAGESIZE));
	double utime = std::stod(f[11]), stime = std::stod(f[12]);
	double start = std::stod(f[19]);
	double vsize = std::stod(f[20]), rss = std::stod(f[21]);
	out.push_back(single("process_cpu_seconds_total", "Total user and system CPU time spent in seconds.",
	                     prometheus::MetricType::Counter, (utime + stime) / ticks));
	out.push_back(single("process_resident_memory_bytes", "Resident memory size in bytes.",
	                     prometheus::MetricType::Gauge, rss * page));
	out.push_back(single("process_virtual_memory_bytes", "Virtual memory size in bytes.",
	                     prometheus::MetricType::Gauge, vsize));
	std::ifstream sys("/proc/stat");
	for (std::string line; std::getline(sys, line);) {
		if (line.starts_with("btime ")) {
			out.push_back(single("process_start_time_seconds",
			                     "Start time of the process since unix epoch in seconds.",
			                     prometheus::MetricType::Gauge, std::stod(line.substr(6)) + start / ticks));
			break;
		}
	}
	if (DIR* d = opendir("/proc/self/fd")) {
		double n = 0;
		while (auto* e = readdir(d)) {
			if (e->d_name[0] != '.') {
				n++;
			}
		}
		closedir(d);
		out.push_back(single("process_open_fds", "Number of open file descriptors.",
		                     prometheus::MetricType::Gauge, n - 1));
	}
	rlimit rl{};
	if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
		out.push_back(single("process_max_fds", "Maximum number of open file descriptors.",
		                     prometheus::MetricType::Gauge, static_cast<double>(rl.rlim_cur)));
	}
}

}  // namespace

Registry::Registry(bool process_metrics) : prom_(std::make_shared<prometheus::Registry>()), process_(process_metrics) {}

prometheus::Family<prometheus::Counter>& Registry::counter_family(const std::string& name, const std::string& help) {
	return prometheus::BuildCounter().Name(name).Help(help).Register(*prom_);
}

prometheus::Counter& Registry::counter(const std::string& name, const std::string& help,
                                       const std::map<std::string, std::string>& labels) {
	return counter_family(name, help).Add(labels);
}

prometheus::Family<prometheus::Gauge>& Registry::gauge_family(const std::string& name, const std::string& help) {
	return prometheus::BuildGauge().Name(name).Help(help).Register(*prom_);
}

prometheus::Gauge& Registry::gauge(const std::string& name, const std::string& help,
                                   const std::map<std::string, std::string>& labels) {
	return gauge_family(name, help).Add(labels);
}

prometheus::Histogram& Registry::histogram(const std::string& name, const std::vector<double>& buckets,
                                           const std::string& help) {
	return prometheus::BuildHistogram().Name(name).Help(help).Register(*prom_).Add({}, buckets);
}

void Registry::gauge_func(const std::string& name, const std::string& help, std::function<double()> fn) {
	std::lock_guard lock(mu_);
	funcs_.push_back(Func{name, help, prometheus::MetricType::Gauge, std::move(fn)});
}

void Registry::counter_func(const std::string& name, const std::string& help, std::function<double()> fn) {
	std::lock_guard lock(mu_);
	funcs_.push_back(Func{name, help, prometheus::MetricType::Counter, std::move(fn)});
}

std::vector<prometheus::MetricFamily> Registry::gather() const {
	auto out = prom_->Collect();
	{
		std::lock_guard lock(mu_);
		for (const auto& f : funcs_) {
			out.push_back(single(f.name, f.help, f.type, f.fn()));
		}
	}
	if (process_) {
		process_families(out);
	}
	return out;
}

std::string Registry::text() const {
	std::ostringstream ss;
	prometheus::TextSerializer().Serialize(ss, gather());
	return ss.str();
}

std::vector<double> exponential_buckets(double start, double factor, int count) {
	std::vector<double> out;
	out.reserve(static_cast<size_t>(count));
	for (int i = 0; i < count; i++) {
		out.push_back(start * std::pow(factor, i));
	}
	return out;
}

void decode_fields(config::Fields& f, AdminConfig& c) {
	f("listen", c.listen)("check_timeout", c.check_timeout);
}

Admin::Admin(AdminConfig cfg, const Registry& reg, log::Logger log)
    : cfg_(std::move(cfg)), reg_(reg), log_(std::move(log)) {
	if (cfg_.check_timeout <= Duration(0)) {
		cfg_.check_timeout = 2s;
	}
}

Admin::~Admin() = default;

void Admin::add_readiness_check(const std::string& name, Check c) {
	std::lock_guard lock(mu_);
	checks_[name] = std::move(c);
}

std::pair<int, std::string> Admin::ready() {
	std::map<std::string, Check> checks;
	{
		std::lock_guard lock(mu_);
		checks = checks_;
	}
	int status = 200;
	nlohmann::json result = nlohmann::json::object();
	for (auto& [name, check] : checks) {
		try {
			check(cfg_.check_timeout);
			result[name] = "ok";
		} catch (const std::exception& e) {
			status = 503;
			result[name] = e.what();
		}
	}
	return {status, result.dump()};
}

void Admin::run(std::stop_token st) {
	http::ServerConfig hc;
	hc.listen = cfg_.listen;
	hc.read_timeout = 5s;
	hc.write_timeout = 30s;
	http::Server srv("admin", hc, log_);
	auto& s = srv.server();
	s.Get("/metrics", [this](const httplib::Request&, httplib::Response& res) {
		res.set_content(reg_.text(), "text/plain; version=0.0.4; charset=utf-8");
	});
	s.Get("/healthz", [](const httplib::Request&, httplib::Response& res) { res.status = 200; });
	s.Get("/readyz", [this](const httplib::Request&, httplib::Response& res) {
		auto [status, body] = ready();
		http::write_json(res, status, body);
	});
	srv.run(st);
}

}  // namespace etrace::telemetry
