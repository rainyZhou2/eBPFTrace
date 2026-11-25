#include "query/service.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace etrace::query {

std::vector<std::string> chain_path(const storage::ChainValue& v) {
	std::vector<std::string> path;
	path.reserve(1 + v.stack.size() + v.chain.size() * 4);
	path.push_back(v.comm);
	path.insert(path.end(), v.stack.rbegin(), v.stack.rend());
	for (const auto& h : v.chain) {
		path.emplace_back(kWakeSeparator);
		path.push_back(std::string(kHopPrefix) + h.comm);
		path.insert(path.end(), h.wake_stack.begin(), h.wake_stack.end());
		if (!h.wait_stack.empty()) {
			path.push_back(std::string(kWaitPrefix) + h.comm);
			path.insert(path.end(), h.wait_stack.begin(), h.wait_stack.end());
		}
	}
	return path;
}

std::vector<flame::Line> Service::lines(const FlameRequest& req) {
	auto vals = reader_.offcpu_chains(storage::ChainQuery{req.filter, req.depth, req.limit});
	std::vector<flame::Line> out;
	out.reserve(vals.size());
	for (const auto& v : vals) {
		out.push_back(flame::Line{chain_path(v), v.total_ns});
	}
	return out;
}

std::unique_ptr<flame::Node> Service::tree(const FlameRequest& req) {
	auto root = flame::Node::root();
	for (const auto& l : lines(req)) {
		root->add(l.path, l.value);
	}
	root->sort();
	return root;
}

pprof::Profile Service::profile(const FlameRequest& req) {
	auto vals = reader_.offcpu_chains(storage::ChainQuery{req.filter, req.depth, req.limit});
	return offcpu_profile(vals, req.filter.from, req.filter.to);
}

pprof::Profile offcpu_profile(const std::vector<storage::ChainValue>& vals, Time from, Time to,
                              const std::string& sample_type) {
	pprof::Profile p;
	p.sample_types = {{sample_type, "nanoseconds"}, {"waits", "count"}};
	p.period_type = {sample_type, "nanoseconds"};
	p.period = 1;
	p.time_nanos = unix_nano(from);
	p.duration_nanos = (to - from).count();
	for (const auto& v : vals) {
		p.samples.push_back(pprof::Sample{flame::reverse(chain_path(v)),
		                                  {static_cast<int64_t>(v.total_ns), static_cast<int64_t>(v.count)},
		                                  {{"comm", v.comm}}});
	}
	return p;
}

std::vector<CausePath> Service::causes(const storage::Filter& f, int depth, int limit) {
	return collapse_causes(reader_.offcpu_chains(storage::ChainQuery{f, depth, 0}), limit);
}

namespace {

class CausePaths {
public:
	void add(const std::string& comm, const std::vector<std::string>& stack, const std::vector<storage::Hop>& chain,
	         uint64_t total_ns, uint64_t count) {
		std::vector<std::string> path{comm};
		std::vector<uint64_t> waits;
		std::string key = comm;
		auto blocked = blocked_frame(stack);
		auto root = blocked;
		auto evidence = CauseEvidence::blocked_site;
		for (const auto& h : chain) {
			path.push_back(h.comm);
			waits.push_back(h.wait_ns);
			key += '\0';
			key += has_overlapping_wait(h) ? '1' : '0';
			key += h.comm;
			const bool has_wait_stack = !h.wait_stack.empty();
			root = h.comm + ": " + blocked_frame(has_wait_stack ? h.wait_stack : h.wake_stack);
			evidence = path.size() == 2 && !has_wait_stack ? CauseEvidence::observed_wakeup
			                                               : CauseEvidence::inferred_upstream;
		}
		key += '\x01';
		key += static_cast<char>(evidence);
		key += root;
		auto [it, inserted] = indices_.try_emplace(std::move(key), paths_.size());
		if (inserted) {
			paths_.push_back(CausePath{std::move(path), std::move(blocked), std::move(root), 0, 0,
			                           std::vector<uint64_t>(waits.size()), evidence});
		}
		auto& c = paths_[it->second];
		c.total_ns += total_ns;
		c.count += count;
		for (size_t i = 0; i < waits.size() && i < c.hops_wait.size(); i++) {
			c.hops_wait[i] += waits[i];
		}
	}

	std::vector<CausePath> finish(int limit) && {
		std::stable_sort(paths_.begin(), paths_.end(),
		                 [](const auto& a, const auto& b) { return a.total_ns > b.total_ns; });
		return storage::limit(std::move(paths_), limit);
	}

private:
	std::unordered_map<std::string, size_t> indices_;
	std::vector<CausePath> paths_;
};

}  // namespace

std::vector<CausePath> collapse_causes(const std::vector<storage::ChainValue>& vals, int limit) {
	CausePaths causes;
	for (const auto& v : vals) {
		causes.add(v.comm, v.stack, v.chain, v.total_ns, v.count);
	}
	return std::move(causes).finish(limit);
}

std::string blocked_frame(const std::vector<std::string>& stack) {
	static const std::unordered_set<std::string_view> generic = {
	        "schedule",
	        "__schedule",
	        "schedule_preempt_disabled",
	        "schedule_timeout",
	        "schedule_hrtimeout_range",
	        "schedule_hrtimeout_range_clock",
	        "io_schedule",
	        "try_to_wake_up",
	        "wake_up_process",
	        "wake_up_q",
	        "wake_up_state",
	        "__wake_up_common",
	        "__wake_up_common_lock",
	        "__wake_up",
	        "__wake_up_sync_key",
	        "autoremove_wake_function",
	        "default_wake_function",
	        "pollwake",
	};
	for (const auto& f : stack) {
		if (f.empty() || f == "[unknown]") {
			continue;
		}
		std::string_view name = f;
		if (auto i = name.rfind('`'); i != std::string_view::npos) {
			name = name.substr(i + 1);
		}
		if (generic.contains(name)) {
			continue;
		}
		return f;
	}
	if (!stack.empty() && !stack[0].empty()) {
		return stack[0];
	}
	return "[unknown]";
}

namespace {

Duration clip(Time s, Time e, Time lo, Time hi) {
	s = std::max(s, lo);
	e = std::min(e, hi);
	if (e <= s) {
		return Duration(0);
	}
	return e - s;
}

}  // namespace

TraceReport Service::trace_report(tracing::Backend* backend, const std::string& trace_id, const storage::Filter& f,
                                  int limit) {
	storage::EventQuery eq;
	eq.filter = f;
	eq.trace_id = trace_id;
	TraceReport rep;
	rep.trace_id = trace_id;
	std::vector<tracing::Span> spans;
	if (backend == nullptr) {
		rep.partial = "no trace backend configured";
	} else {
		try {
			spans = backend->trace(trace_id);
		} catch (const std::exception& e) {
			rep.partial = std::string("trace backend: ") + e.what();
		}
	}
	std::unordered_map<std::string, size_t> idx;
	rep.spans.reserve(spans.size());
	for (size_t i = 0; i < spans.size(); i++) {
		rep.spans.push_back(SpanOffCPU{spans[i], static_cast<uint64_t>(spans[i].duration().count()), 0, 0, {}});
		idx[spans[i].span_id] = i;
	}
	CausePaths causes;
	size_t detail_count = 0;
	reader_.visit_events(eq, [&](storage::Event e) {
		const auto duration = static_cast<uint64_t>((e.end - e.start).count());
		rep.offcpu_total_ns += duration;
		causes.add(e.comm, e.stack, e.chain, duration, 1);
		const bool keep_detail = limit <= 0 || detail_count < static_cast<size_t>(limit);
		detail_count += keep_detail;
		auto it = idx.find(e.span_id);
		if (it == idx.end()) {
			if (keep_detail) {
				rep.unattributed.push_back(std::move(e));
			}
			return;
		}
		size_t i = it->second;
		auto d = static_cast<uint64_t>(
		        clip(e.start, e.end, rep.spans[i].span.start, rep.spans[i].span.end).count());
		rep.spans[i].self_offcpu_ns += d;
		if (keep_detail) {
			rep.spans[i].events.push_back(std::move(e));
		}
		size_t j = i;
		for (size_t seen = 0; seen <= spans.size(); seen++) {
			rep.spans[j].offcpu_ns += d;
			auto p = idx.find(rep.spans[j].span.parent_id);
			if (p == idx.end()) {
				break;
			}
			j = p->second;
		}
	});
	rep.causes = std::move(causes).finish(0);
	std::stable_sort(rep.spans.begin(), rep.spans.end(),
	                 [](const auto& a, const auto& b) { return a.span.start < b.span.start; });
	return rep;
}

Json to_json(const CausePath& c) {
	Json j;
	j["path"] = c.path;
	j["blocked_in"] = c.blocked;
	j["root_cause"] = c.root;
	switch (c.evidence) {
		case CauseEvidence::blocked_site:
			j["cause_evidence"] = "blocked_site";
			break;
		case CauseEvidence::observed_wakeup:
			j["cause_evidence"] = "observed_wakeup";
			break;
		case CauseEvidence::inferred_upstream:
			j["cause_evidence"] = "inferred_upstream";
			break;
	}
	j["total_ns"] = c.total_ns;
	j["count"] = c.count;
	j["hop_wait_ns"] = c.hops_wait;
	return j;
}

Json to_json(const std::vector<CausePath>& cs) {
	auto arr = Json::array();
	for (const auto& c : cs) {
		arr.push_back(to_json(c));
	}
	return arr;
}

Json to_json(const TraceReport& r) {
	Json j;
	j["trace_id"] = r.trace_id;
	if (!r.spans.empty()) {
		auto& arr = j["spans"] = Json::array();
		for (const auto& s : r.spans) {
			Json sj;
			sj["span_id"] = s.span.span_id;
			if (!s.span.parent_id.empty()) {
				sj["parent_span_id"] = s.span.parent_id;
			}
			sj["name"] = s.span.name;
			if (!s.span.service.empty()) {
				sj["service"] = s.span.service;
			}
			sj["start"] = format_rfc3339nano(s.span.start);
			sj["end"] = format_rfc3339nano(s.span.end);
			if (!s.span.attrs.empty()) {
				sj["attributes"] = s.span.attrs;
			}
			sj["duration_ns"] = s.duration_ns;
			sj["offcpu_ns"] = s.offcpu_ns;
			sj["self_offcpu_ns"] = s.self_offcpu_ns;
			if (!s.events.empty()) {
				sj["events"] = to_json(s.events);
			}
			arr.push_back(std::move(sj));
		}
	}
	if (!r.unattributed.empty()) {
		j["unattributed_events"] = to_json(r.unattributed);
	}
	j["offcpu_total_ns"] = r.offcpu_total_ns;
	j["causes"] = to_json(r.causes);
	if (!r.partial.empty()) {
		j["partial"] = r.partial;
	}
	return j;
}

}  // namespace etrace::query
