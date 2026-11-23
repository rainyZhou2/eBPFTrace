#include "agent/processor/container.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

namespace fs = std::filesystem;

namespace etrace::agent::container {

namespace {

bool is_hex(char c) {
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

// find_container_id returns the leftmost run of 64 lower-case hex digits.
std::string find_container_id(const std::string& line) {
	size_t run = 0;
	for (size_t i = 0; i < line.size(); i++) {
		run = is_hex(line[i]) ? run + 1 : 0;
		if (run == 64) {
			return line.substr(i - 63, 64);
		}
	}
	return "";
}

// find_pod_uid matches pod<8>-<4>-<4>-<4>-<12> with '-' or '_' separators.
std::string find_pod_uid(const std::string& line) {
	static constexpr int groups[] = {8, 4, 4, 4, 12};
	for (size_t start = line.find("pod"); start != std::string::npos; start = line.find("pod", start + 1)) {
		size_t i = start + 3;
		std::string uid;
		bool ok = true;
		for (int g = 0; g < 5 && ok; g++) {
			if (g > 0) {
				if (i < line.size() && (line[i] == '-' || line[i] == '_')) {
					uid += '-';
					i++;
				} else {
					ok = false;
					break;
				}
			}
			for (int k = 0; k < groups[g]; k++, i++) {
				if (i >= line.size() || !is_hex(line[i])) {
					ok = false;
					break;
				}
				uid += line[i];
			}
		}
		if (ok) {
			return uid;
		}
	}
	return "";
}

std::string read_file(const std::string& path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		return "";
	}
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

std::string join(const std::string& a, const std::string& b) {
	return (fs::path(a) / fs::path(b).relative_path()).string();
}

}  // namespace

void decode_fields(config::Fields& f, Options& o) {
	f("cache_size", o.cache_size)("cache_ttl", o.cache_ttl)("host_root", o.host_root)("runtime_dirs",
	                                                                                  o.runtime_dirs);
}

std::pair<std::string, std::string> parse_cgroup(std::istream& in) {
	std::string container_id, pod_uid;
	for (std::string line; std::getline(in, line);) {
		if (pod_uid.empty()) {
			pod_uid = find_pod_uid(line);
		}
		if (container_id.empty()) {
			container_id = find_container_id(line);
		}
	}
	return {container_id, pod_uid};
}

std::optional<model::PodRef> parse_runtime_config(const std::string& raw) {
	// Keys the kubelet's container runtimes record for each container: CRI
	// annotations in the OCI config written by containerd, the same names
	// with "pod" spelling by CRI-O, and Docker labels set by cri-dockerd.
	struct Keys {
		const char *uid, *name, *ns, *container;
	};
	static constexpr Keys pod_keys[] = {
	        {"io.kubernetes.cri.sandbox-uid", "io.kubernetes.cri.sandbox-name",
	         "io.kubernetes.cri.sandbox-namespace", "io.kubernetes.cri.container-name"},
	        {"io.kubernetes.pod.uid", "io.kubernetes.pod.name", "io.kubernetes.pod.namespace",
	         "io.kubernetes.container.name"},
	};
	nlohmann::json j = nlohmann::json::parse(raw, nullptr, false);
	if (j.is_discarded() || !j.is_object()) {
		return std::nullopt;
	}
	std::vector<const nlohmann::json*> sets;
	if (auto it = j.find("annotations"); it != j.end() && it->is_object()) {
		sets.push_back(&*it);
	}
	if (auto c = j.find("Config"); c != j.end() && c->is_object()) {
		if (auto l = c->find("Labels"); l != c->end() && l->is_object()) {
			sets.push_back(&*l);
		}
	}
	auto get = [](const nlohmann::json& kv, const char* k) -> std::string {
		auto it = kv.find(k);
		return it != kv.end() && it->is_string() ? it->get<std::string>() : "";
	};
	for (const auto* kv : sets) {
		for (const auto& k : pod_keys) {
			if (get(*kv, k.ns).empty() || get(*kv, k.name).empty()) {
				continue;
			}
			return model::PodRef{get(*kv, k.uid), get(*kv, k.name), get(*kv, k.ns), get(*kv, k.container)};
		}
	}
	return std::nullopt;
}

Processor::Processor(std::string proc_root, Options o)
    : proc_root_(std::move(proc_root)),
      host_root_(o.host_root.empty() ? join(proc_root_, "1/root") : o.host_root),
      runtime_dirs_(std::move(o.runtime_dirs)),
      by_pid_(static_cast<size_t>(o.cache_size), o.cache_ttl),
      by_container_(static_cast<size_t>(o.cache_size), o.cache_ttl) {}

void Processor::process(model::Batch& b) {
	for (auto& s : b.offcpu) {
		tag(s.task);
		for (auto& h : s.chain) {
			tag(h.task);
		}
	}
	for (auto& e : b.events) {
		tag(e.task);
		for (auto& h : e.chain) {
			tag(h.task);
		}
	}
}

void Processor::tag(model::Task& t) {
	if (!t.container_id.empty() || t.pid == 0) {
		return;
	}
	auto id = lookup(t.pid);
	t.container_id = id.container_id;
	t.pod = id.pod;
}

Processor::Identity Processor::lookup(uint32_t pid) {
	if (auto id = by_pid_.get(pid)) {
		return *id;
	}
	Identity id;
	std::ifstream in(join(proc_root_, std::format("{}/cgroup", pid)));
	if (in) {
		auto [cid, pod_uid] = parse_cgroup(in);
		id.container_id = cid;
		if (!cid.empty()) {
			id.pod = pod(cid);
		}
		if (id.pod.uid.empty()) {
			id.pod.uid = pod_uid;
		}
	}
	by_pid_.add(pid, id);
	return id;
}

model::PodRef Processor::pod(const std::string& container_id) {
	if (auto ref = by_container_.get(container_id)) {
		return *ref;
	}
	model::PodRef ref;
	for (const auto& pattern : runtime_dirs_) {
		std::string p = pattern;
		for (size_t pos; (pos = p.find("%s")) != std::string::npos;) {
			p.replace(pos, 2, container_id);
		}
		auto raw = read_file(join(host_root_, p));
		if (raw.empty()) {
			continue;
		}
		if (auto r = parse_runtime_config(raw)) {
			ref = *r;
			break;
		}
	}
	by_container_.add(container_id, ref);
	return ref;
}

}  // namespace etrace::agent::container
