#include "doctor/checks.h"

#include <sys/utsname.h>

#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace etrace::doctor {

namespace {

// Capability numbers from linux/capability.h.
constexpr int kCapSysPtrace = 19;
constexpr int kCapSysAdmin = 21;
constexpr int kCapPerfmon = 38;
constexpr int kCapBPF = 39;

bool kernel_version(const std::string& release, int& major, int& minor) {
	auto dot = release.find('.');
	if (dot == std::string::npos) {
		return false;
	}
	auto rest = std::string_view(release).substr(dot + 1);
	auto [p1, e1] = std::from_chars(release.data(), release.data() + dot, major);
	if (e1 != std::errc() || p1 != release.data() + dot) {
		return false;
	}
	auto end = rest.find_first_not_of("0123456789");
	auto digits = rest.substr(0, end);
	if (digits.empty()) {
		return false;
	}
	auto [p2, e2] = std::from_chars(digits.data(), digits.data() + digits.size(), minor);
	return e2 == std::errc();
}

Check kernel_check(const std::string& release) {
	int major = 0, minor = 0;
	if (!kernel_version(release, major, minor)) {
		return {"kernel", Status::warn, std::format("cannot parse release \"{}\"", release)};
	}
	if (major < 5 || (major == 5 && minor < 8)) {
		return {"kernel", Status::fail, release + ": Linux 5.8 or later is required"};
	}
	if (major == 5 && minor < 17) {
		return {"kernel", Status::warn,
		        release + ": user stacks fall back to frame pointers (DWARF walker needs 5.17)"};
	}
	return {"kernel", Status::ok, release};
}

std::vector<Check> privilege_checks(const std::optional<std::bitset<64>>& caps) {
	auto has = [&](int c) { return caps && caps->test(static_cast<size_t>(c)); };
	std::vector<Check> out;
	if (has(kCapSysAdmin) || (has(kCapBPF) && has(kCapPerfmon))) {
		out.push_back({"privileges", Status::ok, "can load BPF programs"});
	} else {
		out.push_back({"privileges", Status::fail, "needs root, or CAP_BPF and CAP_PERFMON"});
	}
	if (!has(kCapSysPtrace) && !has(kCapSysAdmin)) {
		out.push_back(
		        {"process access", Status::warn,
		         "without CAP_SYS_PTRACE, binaries of other users' processes cannot be read; their frames "
		         "stay unsymbolized"});
	}
	return out;
}

}  // namespace

const char* to_string(Status s) {
	switch (s) {
		case Status::ok:
			return "ok";
		case Status::warn:
			return "warn";
		case Status::fail:
			return "fail";
	}
	return "ok";
}

bool failed(const std::vector<Check>& checks) {
	for (const auto& c : checks) {
		if (c.status == Status::fail) {
			return true;
		}
	}
	return false;
}

std::vector<Check> preflight(const Host& h) {
	if (h.os != "linux") {
		return {{"platform", Status::fail, "etrace doctor needs Linux; this is " + h.os}};
	}
	std::vector<Check> out{kernel_check(h.kernel)};
	if (h.btf) {
		out.push_back({"kernel BTF", Status::ok, "/sys/kernel/btf/vmlinux present"});
	} else {
		out.push_back({"kernel BTF", Status::fail,
		               "/sys/kernel/btf/vmlinux missing; use a kernel built with CONFIG_DEBUG_INFO_BTF=y"});
	}
	auto priv = privilege_checks(h.cap_eff);
	out.insert(out.end(), priv.begin(), priv.end());
	if (h.target_pid != 0) {
		if (h.target_ok) {
			out.push_back({"target process", Status::ok, std::format("pid {}", h.target_pid)});
		} else {
			out.push_back({"target process", Status::fail, std::format("pid {} not found", h.target_pid)});
		}
	}
	return out;
}

std::optional<std::bitset<64>> parse_cap_eff(const std::string& status) {
	std::istringstream in(status);
	for (std::string line; std::getline(in, line);) {
		if (!line.starts_with("CapEff:")) {
			continue;
		}
		auto v = line.substr(7);
		auto b = v.find_first_not_of(" \t");
		auto e = v.find_last_not_of(" \t\r");
		if (b == std::string::npos) {
			return std::nullopt;
		}
		v = v.substr(b, e - b + 1);
		uint64_t n;
		auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), n, 16);
		if (ec != std::errc() || p != v.data() + v.size()) {
			return std::nullopt;
		}
		return std::bitset<64>(n);
	}
	return std::nullopt;
}

Host probe_host(const std::string& proc_root, uint32_t pid) {
	Host h;
	h.os = "linux";
	utsname u{};
	if (uname(&u) == 0) {
		h.kernel = u.release;
	}
	h.target_pid = pid;
	std::error_code ec;
	h.btf = fs::exists("/sys/kernel/btf/vmlinux", ec);
	std::ifstream in(fs::path(proc_root) / "self" / "status");
	if (in) {
		std::ostringstream ss;
		ss << in.rdbuf();
		h.cap_eff = parse_cap_eff(ss.str());
	}
	if (pid != 0) {
		h.target_ok = fs::exists(fs::path(proc_root) / std::to_string(pid), ec);
	}
	return h;
}

std::vector<Check> runtime_checks(const std::vector<prometheus::MetricFamily>& families) {
	std::map<std::string, double> sums;
	std::string mode;
	for (const auto& mf : families) {
		for (const auto& m : mf.metric) {
			double v = m.gauge.value + m.counter.value;
			sums[mf.name] += v;
			if (mf.name == "etrace_offcpu_user_unwinder" && v == 1) {
				for (const auto& l : m.label) {
					if (l.name == "mode") {
						mode = l.value;
					}
				}
			}
		}
	}
	std::vector<Check> out;
	if (mode == "dwarf") {
		out.push_back({"user stacks", Status::ok, "walked with .eh_frame tables"});
	} else if (mode == "frame_pointer") {
		out.push_back({"user stacks", Status::warn,
		               "walked with frame pointers; callers in code built without them are missing"});
	}
	if (auto n = sums["etrace_offcpu_bpf_drops"]; n > 0) {
		out.push_back({"kernel maps", Status::warn,
		               std::format("{:.0f} updates dropped because BPF maps were full; totals are low", n)});
	}
	if (auto n = sums["etrace_offcpu_events_dropped_total"]; n > 0) {
		out.push_back({"events", Status::warn, std::format("{:.0f} slow events dropped", n)});
	}
	if (auto n = sums["etrace_unwind_table_full_total"]; n > 0) {
		out.push_back({"unwind tables", Status::warn,
		               std::format("{:.0f} objects did not fit the unwind table and use frame pointers", n)});
	}
	if (sums["etrace_offcpu_gpu_unsupported"] > 0) {
		out.push_back(
		        {"GPU waits", Status::warn,
		         "not recorded: uprobe attach cookies need Linux 5.15; CUDA waits that spin stay invisible"});
	} else if (auto n = sums["etrace_offcpu_gpu_libraries"]; n > 0) {
		out.push_back({"GPU waits", Status::ok, std::format("probing {:.0f} CUDA and NCCL libraries", n)});
		if (sums["etrace_offcpu_gpu_launch_tracking"] == 0) {
			out.push_back({"GPU kernels", Status::warn,
			               "launch tracking stopped above gpu.max_launch_rate; waits name no kernels"});
		}
	}
	return out;
}

}  // namespace etrace::doctor
