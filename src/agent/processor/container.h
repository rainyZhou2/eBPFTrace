#pragma once

#include <istream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "agent/plugin.h"
#include "platform/lru.h"

namespace etrace::agent::container {

struct Options {
	int cache_size = 8192;
	Duration cache_ttl = 5min;
	// host_root is where the host filesystem is visible; by default the
	// root of host PID 1, which needs hostPID but no extra mounts.
	std::string host_root;
	// runtime_dirs are searched for the OCI config of a container. "%s" is
	// replaced by the container ID.
	std::vector<std::string> runtime_dirs = {
	        "/run/containerd/io.containerd.runtime.v2.task/k8s.io/%s/config.json",
	        "/run/containers/storage/overlay-containers/%s/userdata/config.json",
	        "/var/lib/docker/containers/%s/config.v2.json",
	};
};

void decode_fields(config::Fields& f, Options& o);

// parse_cgroup returns the container ID and, under Kubernetes, the pod UID
// found in /proc/<pid>/cgroup. Both the cgroupfs ("pod<uid>") and systemd
// ("pod<uid with underscores>") drivers are handled.
std::pair<std::string, std::string> parse_cgroup(std::istream& in);

// parse_runtime_config reads pod identity from an OCI runtime config.json
// or a Docker config.v2.json. Sandbox (pause) containers carry the pod but
// no container name.
std::optional<model::PodRef> parse_runtime_config(const std::string& raw);

// Processor tags tasks with their container ID and Kubernetes pod.
class Processor : public agent::Processor {
public:
	Processor(std::string proc_root, Options o);
	std::string name() const override { return "container"; }
	void process(model::Batch& b) override;

private:
	struct Identity {
		std::string container_id;
		model::PodRef pod;
	};
	void tag(model::Task& t);
	Identity lookup(uint32_t pid);
	model::PodRef pod(const std::string& container_id);

	std::string proc_root_;
	std::string host_root_;
	std::vector<std::string> runtime_dirs_;
	LruCache<uint32_t, Identity> by_pid_;
	LruCache<std::string, model::PodRef> by_container_;
};

}  // namespace etrace::agent::container
