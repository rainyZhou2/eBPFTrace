#pragma once

#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "doctor/checks.h"
#include "model/model.h"
#include "query/flame.h"
#include "query/service.h"
#include "storage/storage.h"

namespace etrace::doctor {

// BlockedThread is a thread that was still waiting when recording stopped.
struct BlockedThread {
	std::string comm;
	uint32_t pid = 0;
	uint32_t tid = 0;
	Duration blocked_for{0};
	std::string at;
	std::vector<std::string> stack;
	std::string trace_id;
};

struct ProcessTotal {
	std::string comm;
	uint64_t total_ns = 0;
	double share = 0;
};

struct Report {
	std::string host;
	std::string kernel;
	std::string version;
	std::string target;
	Time start{};
	Time end{};
	bool interrupted = false;
	std::vector<Check> checks;
	uint64_t total_ns = 0;
	std::vector<ProcessTotal> processes;
	std::vector<query::CausePath> causes;
	std::vector<storage::Event> slow;
	std::vector<BlockedThread> blocked;
	std::unique_ptr<query::flame::Node> flame;
};

struct BuildRequest {
	storage::Filter filter;
	int depth = 4;
	int top = 20;
	Duration slow_from{0};
	std::vector<model::OffCPUEvent> blocked;
};

// build queries the collected data the same way the query service does, so
// the report reads like the flamegraph, causes and events endpoints.
Report build(storage::Reader& r, const BuildRequest& req);

// write_html renders a self-contained page. Symbol and process names come
// from the traced host and are HTML-escaped; the flame tree is embedded as
// JSON and drawn with textContent only.
void write_html(std::ostream& out, const Report& r);
void write_text(std::ostream& out, const Report& r);

}  // namespace etrace::doctor
