#include "doctor/report.h"

#include <algorithm>

namespace etrace::doctor {

namespace {

double share(uint64_t v, uint64_t total) {
	return total == 0 ? 0 : static_cast<double>(v) / static_cast<double>(total);
}

std::vector<BlockedThread> blocked_threads(const std::vector<model::OffCPUEvent>& evs, const storage::Filter& f,
                                           int top) {
	std::vector<BlockedThread> out;
	for (const auto& e : evs) {
		if (!f.match_task(e.task)) {
			continue;
		}
		auto stack = model::symbols(e.stack);
		out.push_back(BlockedThread{e.task.comm, e.task.pid, e.task.tid, e.duration(),
		                            query::blocked_frame(stack), stack, e.trace.trace_id_hex()});
	}
	std::stable_sort(out.begin(), out.end(),
	                 [](const auto& a, const auto& b) { return a.blocked_for > b.blocked_for; });
	return storage::limit(std::move(out), top);
}

}  // namespace

Report build(storage::Reader& r, const BuildRequest& req) {
	query::Service svc(r);
	Report rep;
	try {
		rep.flame = svc.tree(query::FlameRequest{req.filter, req.depth, 0});
	} catch (const std::exception& e) {
		throw Error(std::string("flamegraph: ") + e.what());
	}
	try {
		rep.causes = svc.causes(req.filter, req.depth, req.top);
	} catch (const std::exception& e) {
		throw Error(std::string("causes: ") + e.what());
	}
	std::vector<storage::Event> events;
	try {
		storage::EventQuery eq;
		eq.filter = req.filter;
		eq.min_duration = req.slow_from;
		events = r.events(eq);
	} catch (const std::exception& e) {
		throw Error(std::string("events: ") + e.what());
	}
	std::stable_sort(events.begin(), events.end(),
	                 [](const auto& a, const auto& b) { return a.end - a.start > b.end - b.start; });
	rep.total_ns = rep.flame->value;
	rep.slow = storage::limit(std::move(events), req.top);
	rep.blocked = blocked_threads(req.blocked, req.filter, req.top);
	size_t n = rep.flame->children.size();
	if (req.top > 0) {
		n = std::min(n, static_cast<size_t>(req.top));
	}
	for (size_t i = 0; i < n; i++) {
		const auto& c = rep.flame->children[i];
		rep.processes.push_back(ProcessTotal{c->name, c->value, share(c->value, rep.flame->value)});
	}
	return rep;
}

}  // namespace etrace::doctor
