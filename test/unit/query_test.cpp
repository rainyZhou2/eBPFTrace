#include <gtest/gtest.h>

#include <sstream>

#include "collector/service.h"
#include "query/flame.h"
#include "query/pprof.h"
#include "query/service.h"
#include "storage/memory/memory.h"
#include "testutil.h"
#include "wire/wire.h"

namespace etrace {
namespace {

using testing::frames;

const std::string kTraceID = "4bf92f3577b34da6a3ce929d0e0e4736";

std::vector<model::Hop> lock_chain() {
	return {
	        model::Hop{model::Task{20, 0, "holder", "", {}},
	                   frames({"futex_wake", "unlock"}),
	                   frames({"io_schedule", "read"}),
	                   250,
	                   {}},
	        model::Hop{model::Task{0, 0, "[hardirq]", "", {}}, frames({"blk_mq_complete"}), {}, 0, {}},
	};
}

struct Seeded {
	Time now = etrace::now();
	storage::memory::Store store{storage::memory::Options{Duration(0)}};
	storage::Filter window;

	Seeded() {
		window.from = now - 1h;
		window.to = now + 1h;
		auto tc = *model::parse_trace_context(kTraceID, "00f067aa0ba902b7");
		model::Batch b;
		b.id = "1";
		b.resource.node = "n1";
		b.window_start = now - 1s;
		b.window_end = now;
		b.offcpu = {
		        model::OffCPUSample{model::Task{10, 0, "api", "", {}}, frames({"futex_wait", "lock", "main"}),
		                            lock_chain(), 300, 3, model::OffCPUReason::unknown},
		        model::OffCPUSample{model::Task{10, 0, "api", "", {}},
		                            frames({"io_schedule", "main"}),
		                            {},
		                            100,
		                            1,
		                            model::OffCPUReason::unknown},
		};
		b.events = {
		        model::OffCPUEvent{model::Task{10, 11, "api", "", {}}, frames({"futex_wait", "lock"}),
		                           lock_chain(), now - 900ms, now - 600ms, model::OffCPUReason::blocked, tc},
		        model::OffCPUEvent{model::Task{10, 11, "api", "", {}},
		                           frames({"io_schedule"}),
		                           {},
		                           now - 500ms,
		                           now - 400ms,
		                           model::OffCPUReason::blocked,
		                           {}},
		};
		store.write_batch(b);
	}
};

std::string join(const std::vector<std::string>& v, const std::string& sep) {
	std::string out;
	for (size_t i = 0; i < v.size(); i++) {
		out += (i ? sep : "") + v[i];
	}
	return out;
}

TEST(Query, MultiHopFlame) {
	Seeded s;
	query::Service svc(s.store);
	auto lines = svc.lines(query::FlameRequest{s.window, 4, 0});
	std::ostringstream out;
	query::flame::write_collapsed(out, lines);
	EXPECT_EQ(out.str(),
	          "api;main;lock;futex_wait;--;<- holder;futex_wake;unlock;waiting: holder;io_schedule;read;--;<- "
	          "[hardirq];blk_mq_complete 300\n"
	          "api;main;io_schedule 100\n");
	lines = svc.lines(query::FlameRequest{s.window, 1, 0});
	EXPECT_EQ(join(lines[0].path, ";").find("hardirq"), std::string::npos) << "depth 1 must drop the second hop";
}

TEST(Query, CausesFollowChainToRoot) {
	Seeded s;
	query::Service svc(s.store);
	auto causes = svc.causes(s.window, 4, 0);
	ASSERT_FALSE(causes.empty());
	const auto& c = causes[0];
	EXPECT_EQ(join(c.path, ">"), "api>holder>[hardirq]");
	EXPECT_EQ(c.root, "[hardirq]: blk_mq_complete");
	EXPECT_EQ(c.total_ns, 300u);
	EXPECT_EQ(c.hops_wait[0], 250u);
	EXPECT_EQ(c.blocked, "futex_wait");
	EXPECT_EQ(query::blocked_frame({"[kernel]`__schedule", "[kernel]`schedule", "[kernel]`futex_wait"}),
	          "[kernel]`futex_wait");
}

class FakeBackend : public tracing::Backend {
public:
	explicit FakeBackend(std::vector<tracing::Span> spans) : spans_(std::move(spans)) {}
	std::vector<tracing::Span> trace(const std::string&) override { return spans_; }

private:
	std::vector<tracing::Span> spans_;
};

TEST(Query, TraceReportAttributesToSpans) {
	Seeded s;
	query::Service svc(s.store);
	FakeBackend backend({
	        tracing::Span{"a2fb4a1d1a96d312", "", "GET /orders", "", s.now - 1s, s.now, {}},
	        tracing::Span{"00f067aa0ba902b7", "a2fb4a1d1a96d312", "load", "", s.now - 1s, s.now - 700ms, {}},
	});
	auto rep = svc.trace_report(&backend, kTraceID, s.window, 100);
	EXPECT_EQ(rep.offcpu_total_ns, static_cast<uint64_t>(Duration(300ms).count()));
	EXPECT_TRUE(rep.unattributed.empty());
	std::map<std::string, query::SpanOffCPU> by_name;
	for (const auto& sp : rep.spans) {
		by_name[sp.span.name] = sp;
	}
	const auto& child = by_name["load"];
	const auto& root = by_name["GET /orders"];
	EXPECT_EQ(child.self_offcpu_ns, static_cast<uint64_t>(Duration(200ms).count()));
	EXPECT_EQ(child.events.size(), 1u);
	EXPECT_EQ(root.offcpu_ns, child.offcpu_ns);
	EXPECT_EQ(root.self_offcpu_ns, 0u);
	ASSERT_EQ(rep.causes.size(), 1u);
	EXPECT_EQ(rep.causes[0].root, "[hardirq]: blk_mq_complete");

	rep = svc.trace_report(nullptr, kTraceID, s.window, 100);
	EXPECT_FALSE(rep.partial.empty());
	EXPECT_EQ(rep.unattributed.size(), 1u);
	auto j = query::to_json(rep);
	EXPECT_EQ(j["partial"], "no trace backend configured");
	EXPECT_FALSE(j.contains("spans"));
}

TEST(Query, ProfileMatchesFlamePaths) {
	Seeded s;
	query::Service svc(s.store);
	auto p = svc.profile(query::FlameRequest{s.window, 4, 0});
	ASSERT_EQ(p.samples.size(), 2u);
	ASSERT_EQ(p.sample_types.size(), 2u);
	const auto& smp = p.samples[0];
	EXPECT_EQ(smp.values[0], 300);
	EXPECT_EQ(smp.values[1], 3);
	EXPECT_EQ(smp.stack.front(), "blk_mq_complete");
	EXPECT_EQ(smp.stack.back(), "api");
}

TEST(Pprof, EncodesProfileProto) {
	query::pprof::Profile p;
	p.sample_types = {{"off_cpu", "nanoseconds"}, {"waits", "count"}};
	p.period_type = {"off_cpu", "nanoseconds"};
	p.period = 1;
	p.samples = {{{"[kernel]`futex_wait", "app`lock", "app`main"}, {3000000, 2}, {{"comm", "api"}}}};
	auto raw = p.marshal();
	for (const char* want : {"off_cpu", "nanoseconds", "waits", "app`lock", "comm", "api"}) {
		EXPECT_NE(raw.find(want), std::string::npos) << want;
	}
	auto gz = p.gzip();
	ASSERT_GE(gz.size(), 2u);
	EXPECT_EQ(static_cast<uint8_t>(gz[0]), 0x1f);
	EXPECT_EQ(static_cast<uint8_t>(gz[1]), 0x8b);
}

TEST(Flame, TreeJsonOmitsEmptyChildren) {
	auto root = query::flame::Node::root();
	root->add({"a", "b"}, 2);
	root->add({"c"}, 5);
	root->sort();
	auto j = query::flame::to_json(*root);
	EXPECT_EQ(j["value"], 7);
	EXPECT_EQ(j["children"][0]["name"], "c");
	EXPECT_FALSE(j["children"][0].contains("children"));
	EXPECT_EQ(query::html_safe_dump(nlohmann::ordered_json{{"x", "<s>&"}}), R"({"x":"\u003cs\u003e\u0026"})");
}

TEST(Memory, DeduplicatesAndFiltersPods) {
	storage::memory::Store st(storage::memory::Options{Duration(0)});
	model::Batch b;
	b.id = "x";
	b.resource.node = "n";
	b.events.push_back(model::OffCPUEvent{model::Task{1, 2, "api", "", model::PodRef{"u", "api-0", "shop", "app"}},
	                                      {},
	                                      {},
	                                      etrace::now() - 1s,
	                                      etrace::now(),
	                                      model::OffCPUReason::blocked,
	                                      {}});
	st.write_batch(b);
	EXPECT_THROW(st.write_batch(b), storage::DuplicateError);
	storage::EventQuery q;
	q.filter.ns = "shop";
	EXPECT_EQ(st.events(q).size(), 1u);
	q.filter.ns = "other";
	EXPECT_TRUE(st.events(q).empty());
}

TEST(Memory, RetentionEvictsOldBatches) {
	Time now = etrace::now();
	storage::memory::Store st(storage::memory::Options{1h}, [&] { return now; });
	model::Batch b;
	b.id = "old";
	b.window_end = now - 2h;
	b.offcpu.push_back({});
	st.write_batch(b);
	b.id = "new";
	b.window_end = now;
	st.write_batch(b);
	b.id = "old";
	EXPECT_NO_THROW(st.write_batch(b)) << "evicted ids must be accepted again";
}

class FakeWriter : public storage::Writer {
public:
	int calls = 0;
	std::function<void()> fail;
	void write_batch(const model::Batch&) override {
		calls++;
		if (fail) {
			fail();
		}
	}
};

std::unique_ptr<collector::Service> new_svc(FakeWriter& w, telemetry::Registry& reg) {
	return std::make_unique<collector::Service>(w, collector::DedupConfig{10, 1min}, collector::Limits{2, 0, 2, 1},
	                                            reg, log::Logger::discard());
}

TEST(Collector, IngestDeduplicatesRetries) {
	FakeWriter w;
	telemetry::Registry reg(false);
	auto svc = new_svc(w, reg);
	model::Batch b;
	b.id = "x";
	b.offcpu.push_back(model::OffCPUSample{{}, {}, {}, 1, 0, {}});
	EXPECT_FALSE(svc->ingest(b));
	EXPECT_TRUE(svc->ingest(b));
	EXPECT_EQ(w.calls, 1);
	EXPECT_EQ(testing::metric(reg, "etrace_collector_batches_total", "result", "duplicate"), 1);
}

TEST(Collector, StorageDuplicateIsSuccess) {
	FakeWriter w;
	w.fail = [] { throw storage::DuplicateError(); };
	telemetry::Registry reg(false);
	auto svc = new_svc(w, reg);
	model::Batch b;
	b.id = "y";
	EXPECT_TRUE(svc->ingest(b));
}

TEST(Collector, RejectsOversizedAndLongChains) {
	FakeWriter w;
	telemetry::Registry reg(false);
	auto svc = new_svc(w, reg);
	model::Batch b;
	b.id = "z";
	b.offcpu.resize(3);
	EXPECT_THROW(svc->ingest(b), collector::InvalidBatch);
	model::Batch c;
	c.id = "c";
	c.events.resize(1);
	c.events[0].chain.resize(2);
	EXPECT_THROW(svc->ingest(c), collector::InvalidBatch);
}

TEST(Collector, FailureIsRetryable) {
	FakeWriter w;
	w.fail = [] { throw Error("down"); };
	telemetry::Registry reg(false);
	auto svc = new_svc(w, reg);
	model::Batch b;
	b.id = "r";
	EXPECT_THROW(svc->ingest(b), Error);
	w.fail = nullptr;
	EXPECT_FALSE(svc->ingest(b));
}

TEST(Wire, RoundTrip) {
	auto tc = *model::parse_trace_context(kTraceID, "00f067aa0ba902b7");
	model::Batch in;
	in.id = "id";
	in.resource = model::Resource{"n", "a", "", {{"k", "v"}}};
	in.window_start = from_unix(1);
	in.window_end = from_unix(2);
	in.offcpu = {model::OffCPUSample{model::Task{1, 2, "c", "", {}},
	                                 {model::Frame{1, "f", "", model::FrameKind::kernel}},
	                                 {model::Hop{model::Task{3, 3, "w", "", {}},
	                                             {model::Frame{2, "", "", model::FrameKind::user}},
	                                             {model::Frame{9, "", "", model::FrameKind::kernel}},
	                                             7,
	                                             tc},
	                                  model::Hop{model::Task{4, 4, "kworker", "", {}}, {}, {}, 0, {}}},
	                                 10,
	                                 1,
	                                 model::OffCPUReason::blocked}};
	in.offcpu[0].chain[0].wake_evidence = model::Evidence::observed;
	in.offcpu[0].chain[0].wait_evidence = model::Evidence::inferred;
	in.offcpu[0].chain[1].wake_evidence = model::Evidence::inferred;
	in.events = {model::OffCPUEvent{model::Task{1, 2, "c", "", model::PodRef{"u", "p", "ns", "c"}},
	                                {},
	                                {},
	                                from_unix_nano(1000000005),
	                                from_unix_nano(1000000900),
	                                model::OffCPUReason::blocked,
	                                tc}};
	etrace::v1::Batch pb;
	wire::to_proto(in, pb);
	auto out = wire::from_proto(pb);
	EXPECT_EQ(out.id, in.id);
	EXPECT_EQ(out.resource.labels, in.resource.labels);
	EXPECT_EQ(out.window_start, in.window_start);
	ASSERT_EQ(out.offcpu.size(), 1u);
	const auto& s = out.offcpu[0];
	EXPECT_EQ(s.stack[0].function, "f");
	ASSERT_EQ(s.chain.size(), 2u);
	EXPECT_EQ(s.chain[0].wait_ns, 7u);
	EXPECT_EQ(s.chain[0].trace, tc);
	EXPECT_EQ(s.chain[0].wake_evidence, model::Evidence::observed);
	EXPECT_EQ(s.chain[0].wait_evidence, model::Evidence::inferred);
	EXPECT_EQ(s.chain[1].wake_evidence, model::Evidence::inferred);
	EXPECT_EQ(s.chain[0].wait_stack[0].address, 9u);
	EXPECT_EQ(s.chain[1].task.comm, "kworker");
	ASSERT_EQ(out.events.size(), 1u);
	EXPECT_EQ(out.events[0].start, in.events[0].start);
	EXPECT_EQ(out.events[0].task.pod, in.events[0].task.pod);
	EXPECT_EQ(out.events[0].trace, tc);

	etrace::v1::Batch empty;
	wire::to_proto(model::Batch{}, empty);
	EXPECT_THROW(wire::from_proto(empty), wire::InvalidBatch);
}

}  // namespace
}  // namespace etrace
