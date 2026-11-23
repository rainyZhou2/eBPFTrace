#include "wire/wire.h"

#include <algorithm>

namespace etrace::wire {

namespace pb = etrace::v1;

namespace {

void task_to_proto(const model::Task& t, pb::Task* out) {
	out->set_pid(t.pid);
	out->set_tid(t.tid);
	out->set_comm(t.comm);
	out->set_container_id(t.container_id);
	if (!t.pod.empty()) {
		auto* p = out->mutable_pod();
		p->set_uid(t.pod.uid);
		p->set_name(t.pod.name);
		p->set_namespace_(t.pod.ns);
		p->set_container(t.pod.container);
	}
}

void set_task(const model::Task& t, auto* msg) {
	if (!t.empty()) {
		task_to_proto(t, msg->mutable_task());
	}
}

model::Task task_from_proto(const pb::Task& t) {
	const auto& p = t.pod();
	return model::Task{t.pid(), t.tid(), t.comm(), t.container_id(),
	                   model::PodRef{p.uid(), p.name(), p.namespace_(), p.container()}};
}

void stack_to_proto(const model::Stack& s, pb::Stack* out) {
	out->mutable_frames()->Reserve(static_cast<int>(s.size()));
	for (const auto& f : s) {
		auto* pf = out->add_frames();
		pf->set_address(f.address);
		pf->set_function(f.function);
		pf->set_module(f.module);
		pf->set_kind(static_cast<pb::FrameKind>(f.kind));
	}
}

model::Stack stack_from_proto(const pb::Stack& s) {
	model::Stack out;
	out.reserve(static_cast<size_t>(s.frames_size()));
	for (const auto& f : s.frames()) {
		out.push_back(
		        model::Frame{f.address(), f.function(), f.module(), static_cast<model::FrameKind>(f.kind())});
	}
	return out;
}

void trace_to_proto(const model::TraceContext& t, pb::TraceContext* out) {
	out->set_trace_id(t.trace_id.data(), t.trace_id.size());
	out->set_span_id(t.span_id.data(), t.span_id.size());
}

model::TraceContext trace_from_proto(const pb::TraceContext& t) {
	model::TraceContext out;
	if (t.trace_id().empty()) {
		return out;
	}
	if (t.trace_id().size() != 16 || (!t.span_id().empty() && t.span_id().size() != 8)) {
		throw InvalidBatch("trace context has invalid length");
	}
	std::copy(t.trace_id().begin(), t.trace_id().end(), out.trace_id.begin());
	std::copy(t.span_id().begin(), t.span_id().end(), out.span_id.begin());
	return out;
}

template <class Msg>
void chain_to_proto(const std::vector<model::Hop>& c, Msg* out) {
	for (const auto& h : c) {
		auto* ph = out->add_chain();
		set_task(h.task, ph);
		if (!h.wake_stack.empty()) {
			stack_to_proto(h.wake_stack, ph->mutable_wake_stack());
		}
		if (!h.wait_stack.empty()) {
			stack_to_proto(h.wait_stack, ph->mutable_wait_stack());
		}
		ph->set_wait_ns(h.wait_ns);
		if (h.trace.valid()) {
			trace_to_proto(h.trace, ph->mutable_trace());
		}
	}
}

template <class Msg>
std::vector<model::Hop> chain_from_proto(const Msg& m) {
	std::vector<model::Hop> out;
	out.reserve(static_cast<size_t>(m.chain_size()));
	for (const auto& h : m.chain()) {
		out.push_back(model::Hop{task_from_proto(h.task()), stack_from_proto(h.wake_stack()),
		                         stack_from_proto(h.wait_stack()), h.wait_ns(), trace_from_proto(h.trace())});
	}
	return out;
}

}  // namespace

void to_proto(const model::Batch& b, pb::Batch& out) {
	out.set_id(b.id);
	auto* r = out.mutable_resource();
	r->set_node(b.resource.node);
	r->set_agent_id(b.resource.agent_id);
	r->set_agent_version(b.resource.agent_version);
	for (const auto& [k, v] : b.resource.labels) {
		(*r->mutable_labels())[k] = v;
	}
	out.set_window_start_unix_nano(unix_nano(b.window_start));
	out.set_window_end_unix_nano(unix_nano(b.window_end));
	out.mutable_offcpu()->Reserve(static_cast<int>(b.offcpu.size()));
	for (const auto& s : b.offcpu) {
		auto* ps = out.add_offcpu();
		set_task(s.task, ps);
		if (!s.stack.empty()) {
			stack_to_proto(s.stack, ps->mutable_stack());
		}
		chain_to_proto(s.chain, ps);
		ps->set_blocked_ns(s.blocked_ns);
		ps->set_count(s.count);
		ps->set_reason(static_cast<pb::OffCPUReason>(s.reason));
	}
	out.mutable_events()->Reserve(static_cast<int>(b.events.size()));
	for (const auto& e : b.events) {
		auto* pe = out.add_events();
		set_task(e.task, pe);
		if (!e.stack.empty()) {
			stack_to_proto(e.stack, pe->mutable_stack());
		}
		chain_to_proto(e.chain, pe);
		pe->set_start_unix_nano(unix_nano(e.start));
		pe->set_end_unix_nano(unix_nano(e.end));
		pe->set_reason(static_cast<pb::OffCPUReason>(e.reason));
		if (e.trace.valid()) {
			trace_to_proto(e.trace, pe->mutable_trace());
		}
		pe->set_gpu_stream(e.gpu.stream);
		pe->set_gpu_launches(e.gpu.launches);
	}
}

model::Batch from_proto(const pb::Batch& p) {
	if (p.id().empty()) {
		throw InvalidBatch("batch id is required");
	}
	if (p.resource().node().empty()) {
		throw InvalidBatch("batch resource node is required");
	}
	if (p.window_end_unix_nano() < p.window_start_unix_nano()) {
		throw InvalidBatch("batch window end precedes start");
	}
	const auto& r = p.resource();
	model::Batch b;
	b.id = p.id();
	b.resource.node = r.node();
	b.resource.agent_id = r.agent_id();
	b.resource.agent_version = r.agent_version();
	for (const auto& [k, v] : r.labels()) {
		b.resource.labels[k] = v;
	}
	b.window_start = from_unix_nano(p.window_start_unix_nano());
	b.window_end = from_unix_nano(p.window_end_unix_nano());
	b.offcpu.reserve(static_cast<size_t>(p.offcpu_size()));
	for (const auto& s : p.offcpu()) {
		b.offcpu.push_back(model::OffCPUSample{task_from_proto(s.task()), stack_from_proto(s.stack()),
		                                       chain_from_proto(s), s.blocked_ns(), s.count(),
		                                       static_cast<model::OffCPUReason>(s.reason())});
	}
	b.events.reserve(static_cast<size_t>(p.events_size()));
	for (const auto& e : p.events()) {
		if (e.end_unix_nano() < e.start_unix_nano()) {
			throw InvalidBatch("event end precedes start");
		}
		b.events.push_back(
		        model::OffCPUEvent{task_from_proto(e.task()), stack_from_proto(e.stack()), chain_from_proto(e),
		                           from_unix_nano(e.start_unix_nano()), from_unix_nano(e.end_unix_nano()),
		                           static_cast<model::OffCPUReason>(e.reason()), trace_from_proto(e.trace()),
		                           model::GPUWait{e.gpu_stream(), e.gpu_launches()}});
	}
	return b;
}

}  // namespace etrace::wire
