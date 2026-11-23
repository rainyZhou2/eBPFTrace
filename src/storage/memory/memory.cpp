#include "storage/memory/memory.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace etrace::storage::memory {

namespace {

void join(std::string& b, const std::vector<std::string>& v) {
	for (size_t i = 0; i < v.size(); i++) {
		if (i > 0) {
			b += '\x01';
		}
		b += v[i];
	}
}

std::string chain_key(const ChainValue& v) {
	std::string b = v.comm;
	b += '\0';
	join(b, v.stack);
	for (const auto& h : v.chain) {
		b += '\0';
		b += h.comm;
		b += '\0';
		join(b, h.wake_stack);
		b += '\0';
		join(b, h.wait_stack);
		b += '\0';
		b += (h.wait_ns > 0 || !h.wait_stack.empty()) ? "true" : "false";
	}
	return b;
}

bool match_resource(const Filter& f, const model::Resource& r) {
	if (!f.node.empty() && f.node != r.node) {
		return false;
	}
	for (const auto& [k, v] : f.labels) {
		auto it = r.labels.find(k);
		if ((it == r.labels.end() ? std::string() : it->second) != v) {
			return false;
		}
	}
	return true;
}

}  // namespace

void decode_fields(config::Fields& f, Options& o) {
	f("retention", o.retention);
}

Store::Store(Options o, std::function<Time()> clock) : opts_(o), now_(std::move(clock)) {}

void Store::write_batch(const model::Batch& b) {
	std::unique_lock lock(mu_);
	evict_locked();
	if (!seen_.insert(b.id).second) {
		throw DuplicateError();
	}
	batches_.push_back(std::make_shared<const model::Batch>(b));
}

void Store::evict_locked() {
	if (opts_.retention <= Duration(0)) {
		return;
	}
	auto cutoff = now_() - opts_.retention;
	std::erase_if(batches_, [&](const auto& b) {
		if (b->window_end > cutoff) {
			return false;
		}
		seen_.erase(b->id);
		return true;
	});
}

template <class Fn>
void Store::each_batch(const Filter& f, Fn&& fn) {
	std::shared_lock lock(mu_);
	for (const auto& b : batches_) {
		if (!is_zero(f.from) && b->window_end < f.from) {
			continue;
		}
		if (!is_zero(f.to) && b->window_start > f.to) {
			continue;
		}
		if (match_resource(f, b->resource)) {
			fn(*b);
		}
	}
}

std::vector<ChainValue> Store::offcpu_chains(const ChainQuery& q) {
	std::unordered_map<std::string, ChainValue> agg;
	each_batch(q.filter, [&](const model::Batch& b) {
		for (const auto& smp : b.offcpu) {
			if (!q.filter.match_task(smp.task)) {
				continue;
			}
			ChainValue v{smp.task.comm, model::symbols(smp.stack), truncate(hops(smp.chain), q.depth), 0,
			             0};
			for (auto& h : v.chain) {
				h.pid = 0;
				h.trace_id.clear();
				h.span_id.clear();
			}
			auto key = chain_key(v);
			auto [it, inserted] = agg.try_emplace(key);
			auto& cur = it->second;
			if (inserted) {
				cur.comm = v.comm;
				cur.stack = v.stack;
				cur.chain = v.chain;
				for (auto& h : cur.chain) {
					h.wait_ns = 0;
				}
			}
			cur.total_ns += smp.blocked_ns;
			cur.count += smp.count;
			for (size_t j = 0; j < cur.chain.size(); j++) {
				cur.chain[j].wait_ns += v.chain[j].wait_ns;
			}
		}
	});
	std::vector<ChainValue> out;
	out.reserve(agg.size());
	for (auto& [_, v] : agg) {
		out.push_back(std::move(v));
	}
	std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.total_ns > b.total_ns; });
	return limit(std::move(out), q.limit);
}

std::vector<Event> Store::events(const EventQuery& q) {
	std::vector<Event> out;
	each_batch(q.filter, [&](const model::Batch& b) {
		for (const auto& e : b.events) {
			if (!q.filter.match_task(e.task) || e.duration() < q.min_duration) {
				continue;
			}
			if (!is_zero(q.filter.from) && e.end < q.filter.from) {
				continue;
			}
			if (!is_zero(q.filter.to) && e.start > q.filter.to) {
				continue;
			}
			if (!q.trace_id.empty() && e.trace.trace_id_hex() != q.trace_id) {
				continue;
			}
			if (!q.span_id.empty() && e.trace.span_id_hex() != q.span_id) {
				continue;
			}
			out.push_back(Event{b.resource.node, e.task.pid, e.task.tid, e.task.comm, e.task.container_id,
			                    e.task.pod.ns, e.task.pod.name, e.task.pod.container,
			                    model::symbols(e.stack), hops(e.chain), e.start, e.end,
			                    model::to_string(e.reason), e.trace.trace_id_hex(), e.trace.span_id_hex(),
			                    e.gpu.stream, e.gpu.launches});
		}
	});
	std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
	return limit(std::move(out), q.limit);
}

}  // namespace etrace::storage::memory
