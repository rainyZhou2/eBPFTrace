#pragma once

#include <bitset>
#include <optional>
#include <string>
#include <vector>

#include <prometheus/metric_family.h>

namespace etrace::doctor {

enum class Status { ok, warn, fail };

const char* to_string(Status s);

struct Check {
	std::string name;
	Status status;
	std::string detail;
};

bool failed(const std::vector<Check>& checks);

// Host is what the preflight checks read from the system, gathered by
// probe_host so the rules stay testable.
struct Host {
	std::string os;
	std::string kernel;
	bool btf = false;
	std::optional<std::bitset<64>> cap_eff;
	uint32_t target_pid = 0;
	bool target_ok = false;
};

// preflight decides whether the probes can run and what will be degraded.
std::vector<Check> preflight(const Host& h);
// parse_cap_eff extracts the effective capability mask from
// /proc/<pid>/status.
std::optional<std::bitset<64>> parse_cap_eff(const std::string& status);
Host probe_host(const std::string& proc_root, uint32_t pid);

// runtime_checks reports degradations the agent exported as metrics during
// the run: the unwinder chosen at load time and data lost to full maps.
std::vector<Check> runtime_checks(const std::vector<prometheus::MetricFamily>& families);

}  // namespace etrace::doctor
