#pragma once

#include <cstdint>
#include <functional>
#include <istream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stop_token>
#include <string>
#include <vector>

#include "agent/source/offcpu/bpf_object.h"
#include "platform/log.h"
#include "platform/time.h"

namespace etrace::agent::offcpu::uprobes {

// Target is a binary to probe. key identifies the file so each library is
// probed once regardless of how many processes map it.
struct Target {
	std::string key;
	std::string path;
};

struct ScanResult {
	std::vector<Target> targets;
	bool complete = true;
};

struct Scanner {
	std::string proc_root;
	std::vector<std::string> patterns;
	std::vector<std::string> paths;

	// scan returns executable mappings whose path matches a configured
	// pattern, plus configured static paths. Paths are resolved through
	// /proc/<pid>/root so binaries in other mount namespaces are reachable.
	// complete is false when /proc itself could not be read.
	ScanResult scan() const;
};

std::vector<std::string> match_maps(std::istream& in, const std::vector<std::string>& patterns);

// Probe is one BPF program attached to a symbol of a target. cookie is
// returned by bpf_get_attach_cookie in the program.
struct Probe {
	std::string prog;
	std::string symbol;
	bool retprobe = false;
	uint64_t cookie = 0;
};

// ProbeFn lists the probes for a target file; it may throw for files it
// cannot read.
using ProbeFn = std::function<std::vector<Probe>(const std::string& path)>;

// single returns a ProbeFn that attaches prog to symbol in every target.
ProbeFn single(std::string symbol, std::string prog);

// Manager keeps the probes of every matching binary found on the host
// attached, and detaches probes whose file is no longer mapped. name labels
// log lines.
class Manager {
public:
	Manager(Scanner sc, ProbeFn probes, std::string name, BpfObject& obj, log::Logger log);

	void run(std::stop_token st, Duration interval);
	void sync();
	// attached returns the number of targets with at least one probe.
	size_t attached();
	void close();

private:
	Scanner scanner_;
	ProbeFn probes_;
	std::string name_;
	BpfObject& obj_;
	log::Logger log_;
	std::mutex mu_;
	std::map<std::string, std::vector<std::unique_ptr<Link>>> links_;
	std::set<std::string> failed_;
};

}  // namespace etrace::agent::offcpu::uprobes
