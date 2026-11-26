#include "storage/storage.h"

namespace etrace::storage {

bool Filter::match_task(const model::Task& t) const {
	return (comm.empty() || comm == t.comm) && (pid == 0 || pid == t.pid) &&
	       (container_id.empty() || container_id == t.container_id) && (ns.empty() || ns == t.pod.ns) &&
	       (pod.empty() || pod == t.pod.name);
}

std::vector<Hop> hops(const std::vector<model::Hop>& c) {
	std::vector<Hop> out;
	out.reserve(c.size());
	for (const auto& h : c) {
		out.push_back(Hop{h.task.comm, h.task.pid, model::symbols(h.wake_stack), model::symbols(h.wait_stack),
		                  h.wait_ns, h.trace.trace_id_hex(), h.trace.span_id_hex(), h.wake_evidence,
		                  h.wait_evidence});
	}
	return out;
}

std::vector<Hop> truncate(std::vector<Hop> c, int depth) {
	if (depth >= 0 && c.size() > static_cast<size_t>(depth)) {
		c.resize(static_cast<size_t>(depth));
	}
	return c;
}

}  // namespace etrace::storage
