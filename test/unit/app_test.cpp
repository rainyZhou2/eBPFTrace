#include <gtest/gtest.h>

#include <sstream>

#include "agent/source/offcpu/options.h"
#include "app/agent.h"
#include "app/collector.h"
#include "app/doctor.h"
#include "app/query.h"
#include "doctor/checks.h"
#include "doctor/report.h"
#include "doctor/session.h"
#include "platform/flags.h"
#include "query/service.h"
#include "storage/clickhouse/clickhouse.h"
#include "storage/memory/memory.h"
#include "testutil.h"

namespace etrace {
namespace {

using doctor::Status;

std::bitset<64> caps(std::initializer_list<int> bits) {
	std::bitset<64> b;
	for (int i : bits) {
		b.set(static_cast<size_t>(i));
	}
	return b;
}

std::optional<Status> status(const std::vector<doctor::Check>& checks, const std::string& name) {
	for (const auto& c : checks) {
		if (c.name == name) {
			return c.status;
		}
	}
	return std::nullopt;
}

TEST(Doctor, Preflight) {
	auto root = caps({21});
	struct Case {
		std::string name;
		doctor::Host host;
		std::map<std::string, std::optional<Status>> want;
		bool fail;
	};
	std::vector<Case> cases{
	        {"not linux", {"darwin"}, {{"platform", Status::fail}}, true},
	        {"modern root",
	         {"linux", "6.1.0-56-amd64", true, root},
	         {{"kernel", Status::ok},
	          {"kernel BTF", Status::ok},
	          {"privileges", Status::ok},
	          {"process access", std::nullopt}},
	         false},
	        {"5.15 falls back", {"linux", "5.15.0-91-generic", true, root}, {{"kernel", Status::warn}}, false},
	        {"too old", {"linux", "4.19.0", true, root}, {{"kernel", Status::fail}}, true},
	        {"no btf", {"linux", "6.8.0", false, root}, {{"kernel BTF", Status::fail}}, true},
	        {"bpf caps without ptrace",
	         {"linux", "6.8.0", true, caps({39, 38})},
	         {{"privileges", Status::ok}, {"process access", Status::warn}},
	         false},
	        {"unprivileged", {"linux", "6.8.0", true, caps({})}, {{"privileges", Status::fail}}, true},
	        {"missing target", {"linux", "6.8.0", true, root, 42, false}, {{"target process", Status::fail}}, true},
	};
	for (const auto& c : cases) {
		auto got = doctor::preflight(c.host);
		for (const auto& [name, want] : c.want) {
			EXPECT_EQ(status(got, name), want) << c.name << ": " << name;
		}
		EXPECT_EQ(doctor::failed(got), c.fail) << c.name;
	}
}

TEST(Doctor, ParseCapEff) {
	auto c = doctor::parse_cap_eff("Name:\tx\nCapInh:\t0000000000000000\nCapEff:\t000001ffffffffff\n");
	ASSERT_TRUE(c);
	EXPECT_TRUE(c->test(39));
	EXPECT_TRUE(c->test(40));
	EXPECT_FALSE(c->test(41));
	EXPECT_FALSE(doctor::parse_cap_eff("Name: x\n"));
}

class FakeSource : public agent::Source {
public:
	std::atomic<int> collects{0};
	bool fail = false;
	std::string name() const override { return "fake"; }
	void start() override {}
	void close() override {}
	void collect(model::Batch& b) override {
		if (fail) {
			throw Error("map gone");
		}
		collects++;
		auto now = etrace::now();
		b.offcpu.push_back(model::OffCPUSample{
		        model::Task{7, 7, "api", "", {}},
		        {model::Frame{0, "futex_wait", "[kernel]", model::FrameKind::kernel},
		         model::Frame{0, "<script>alert(1)</script>", "app", model::FrameKind::user}},
		        {model::Hop{model::Task{0, 0, "worker", "", {}},
		                    {model::Frame{0, "unlock", "app", {}}},
		                    {},
		                    0,
		                    {}}},
		        30000000,
		        3,
		        model::OffCPUReason::blocked});
		b.events.push_back(model::OffCPUEvent{
		        model::Task{7, 8, "api", "", {}},
		        {model::Frame{0, "schedule", "[kernel]", {}}, model::Frame{0, "read_config", "app", {}}},
		        {},
		        now - 40ms,
		        now,
		        model::OffCPUReason::blocked,
		        {}});
	}
};

class RenameProcessor : public agent::Processor {
public:
	std::string name() const override { return "rename"; }
	void process(model::Batch& b) override {
		for (auto& s : b.offcpu) {
			s.task.container_id = "c1";
		}
	}
};

struct SessionRun {
	FakeSource src;
	agent::ProcessorChain chain;
	storage::memory::Store store{storage::memory::Options{Duration(0)}};
	doctor::Window window;

	SessionRun(std::stop_token st, Duration d, Duration interval = 20ms, uint32_t skip = 0) {
		chain.add(std::make_unique<RenameProcessor>());
		doctor::Session s{src, chain, store, {}};
		s.interval = interval;
		s.skip_pid = skip;
		window = s.run(st, d);
	}
};

TEST(Doctor, SessionCollectsUntilDurationAndOnCancel) {
	SessionRun r({}, 70ms);
	EXPECT_FALSE(r.window.interrupted);
	EXPECT_EQ(r.window.batches, r.src.collects.load());
	EXPECT_GE(r.window.batches, 2);
	storage::ChainQuery q;
	q.filter.container_id = "c1";
	auto vals = r.store.offcpu_chains(q);
	ASSERT_EQ(vals.size(), 1u);
	EXPECT_EQ(vals[0].count, static_cast<uint64_t>(3 * r.window.batches));

	SessionRun skip({}, 1ms, 1h, 7);
	EXPECT_EQ(skip.window.batches, 0) << "own process not skipped";

	std::stop_source stop;
	stop.request_stop();
	SessionRun cancelled(stop.get_token(), 1h);
	EXPECT_TRUE(cancelled.window.interrupted);
	EXPECT_EQ(cancelled.window.batches, 1) << "cancelled run must still collect once";
}

TEST(Doctor, SessionReportsCollectFailure) {
	FakeSource src;
	src.fail = true;
	agent::ProcessorChain chain;
	storage::memory::Store store(storage::memory::Options{Duration(0)});
	doctor::Session s{src, chain, store, {}};
	s.interval = 1h;
	EXPECT_THROW(s.run({}, 1ms), Error);
}

TEST(Doctor, BuildAndRender) {
	SessionRun r({}, 30ms);
	auto rep = doctor::build(r.store, doctor::BuildRequest{{}, 4, 10, 20ms, {}});
	EXPECT_EQ(rep.total_ns, static_cast<uint64_t>(r.window.batches) * 30000000);
	ASSERT_EQ(rep.processes.size(), 1u);
	EXPECT_EQ(rep.processes[0].share, 1);
	ASSERT_EQ(rep.causes.size(), 1u);
	EXPECT_EQ(rep.causes[0].root, "worker: app`unlock");
	ASSERT_FALSE(rep.slow.empty());
	EXPECT_EQ(query::blocked_frame(rep.slow[0].stack), "app`read_config");

	std::ostringstream text;
	doctor::write_text(text, rep);
	for (const char* want :
	     {"api <- worker (observed)", "observed wakeup: worker: app`unlock", "app`read_config"}) {
		EXPECT_NE(text.str().find(want), std::string::npos) << want << "\n" << text.str();
	}
	std::ostringstream html;
	doctor::write_html(html, rep);
	auto page = html.str();
	EXPECT_EQ(page.find("<script>alert(1)</script>"), std::string::npos) << "symbol name rendered unescaped";
	EXPECT_NE(page.find("id=\"flame-data\""), std::string::npos);
	EXPECT_NE(page.find("read_config"), std::string::npos);
}

TEST(Doctor, RuntimeChecks) {
	telemetry::Registry reg(false);
	reg.gauge_family("etrace_offcpu_user_unwinder").Add({{"mode", "frame_pointer"}}).Set(1);
	reg.gauge_family("etrace_offcpu_bpf_drops").Add({{"reason", "stack"}}).Set(3);
	auto got = doctor::runtime_checks(reg.gather());
	EXPECT_EQ(status(got, "user stacks"), Status::warn);
	EXPECT_EQ(status(got, "kernel maps"), Status::warn);
}

TEST(Doctor, StillBlockedThreadsAreReported) {
	FakeSource src;
	agent::ProcessorChain chain;
	chain.add(std::make_unique<RenameProcessor>());
	storage::memory::Store store(storage::memory::Options{Duration(0)});
	doctor::Session s{src, chain, store, {}};
	s.interval = 1h;
	s.skip_pid = 7;
	s.blocked = [] {
		auto now = etrace::now();
		return std::vector<model::OffCPUEvent>{
		        {model::Task{9, 10, "db", "", {}},
		         {model::Frame{0, "futex_wait", "[kernel]", {}}},
		         {},
		         now - 1min,
		         now,
		         {},
		         {}},
		        {model::Task{7, 7, "self", "", {}}, {}, {}, now - 1h, now, {}, {}},
		};
	};
	auto w = s.run({}, 1ms);
	ASSERT_EQ(w.blocked.size(), 1u);
	EXPECT_EQ(w.blocked[0].task.comm, "db");
	storage::memory::Store empty(storage::memory::Options{Duration(0)});
	auto rep = doctor::build(empty, doctor::BuildRequest{{}, 4, 5, Duration(0), w.blocked});
	ASSERT_EQ(rep.blocked.size(), 1u);
	EXPECT_GE(rep.blocked[0].blocked_for, 1min);
	EXPECT_EQ(rep.blocked[0].at, "[kernel]`futex_wait");
	std::ostringstream text, page;
	doctor::write_text(text, rep);
	doctor::write_html(page, rep);
	EXPECT_NE(text.str().find("still blocked"), std::string::npos);
	EXPECT_NE(page.str().find("Still blocked"), std::string::npos);
}

TEST(DoctorApp, ParseFlags) {
	std::ostringstream err;
	auto o = app::doctor::parse_flags(
	        {"-duration", "5s", "-pid", "42", "-debuginfod", "https://a,https://b", "-o", ""}, err);
	EXPECT_EQ(o.duration, 5s);
	EXPECT_EQ(o.pid, 42u);
	EXPECT_EQ(o.debuginfod.size(), 2u);
	EXPECT_EQ(o.out, "");
	EXPECT_EQ(o.unwind, "auto");
	for (const auto& bad : std::vector<std::vector<std::string>>{
	             {"-duration", "10ms"}, {"-depth", "5"}, {"-top", "0"}, {"-slow", "1ms", "-min-block", "5ms"}}) {
		EXPECT_THROW(app::doctor::parse_flags(bad, err), Error) << bad[0];
	}
	telemetry::Registry reg(false);
	app::doctor::Options bad;
	bad.debuginfod = {"ftp://x"};
	EXPECT_THROW(app::doctor::processors(bad, agent::Deps{log::Logger::discard(), reg, "/proc"}), Error);
}

TEST(Configs, ShippedConfigsParse) {
	setenv("NODE_NAME", "node-1", 1);
	setenv("CLICKHOUSE_ADDR", "127.0.0.1:9000", 1);
	setenv("JAEGER_QUERY_URL", "http://127.0.0.1:16686", 1);
	setenv("ETRACE_INGEST_TOKEN", "ingest-test", 1);
	setenv("ETRACE_QUERY_TOKEN", "query-test", 1);
	unsetenv("ETRACE_QUERY_USER");
	unsetenv("ETRACE_QUERY_PASSWORD");
	auto dir = std::string(ETRACE_SOURCE_DIR) + "/configs/";
	auto a = config::load<app::agent::Config>(dir + "agent.yaml");
	EXPECT_EQ(a.node, "node-1");
	EXPECT_NE(a.retry.max, Duration(0));
	EXPECT_EQ(a.processors.size(), 2u);
	EXPECT_NO_THROW(config::load<app::collector::Config>(dir + "collector.yaml"));
	EXPECT_NO_THROW(config::load<app::collector::Config>(dir + "standalone-collector.yaml"));
	auto q = config::load<app::query::Config>(dir + "query.yaml");
	EXPECT_EQ(q.max_range, 168h);
	EXPECT_EQ(q.trace_backend.driver, "jaeger");
	// Plugin options are decoded by the driver; check the shipped ones too.
	EXPECT_NO_THROW(a.sources[0].options_as<agent::offcpu::Options>());
	EXPECT_NO_THROW(q.storage.options_as<storage::clickhouse::Options>());
}

TEST(Configs, RejectUnknownTopLevelFields) {
	EXPECT_THROW(config::parse<app::query::Config>("storage: {driver: memory}\nmax_rnage: 1h\n"), Error);
	EXPECT_THROW(config::parse<app::query::Config>("max_range: 1h\n"), Error) << "storage.driver is required";
}

// Runs against a real server when ETRACE_CLICKHOUSE_ADDR is set, e.g.
// docker run -p 9000:9000 clickhouse/clickhouse-server.
TEST(ClickHouse, MigrateExistingTablesAndQueryPods) {
	const char* addr = std::getenv("ETRACE_CLICKHOUSE_ADDR");
	if (!addr) {
		GTEST_SKIP() << "ETRACE_CLICKHOUSE_ADDR not set";
	}
	storage::clickhouse::Options o;
	o.addrs = {addr};
	if (const char* u = std::getenv("ETRACE_CLICKHOUSE_USER")) {
		o.username = u;
	}
	if (const char* p = std::getenv("ETRACE_CLICKHOUSE_PASSWORD")) {
		o.password = p;
	}
	o.database = "etrace_test_" + std::to_string(unix_nano(etrace::now()));
	o.auto_migrate = false;
	storage::clickhouse::Store s(o);
	struct Drop {
		storage::clickhouse::Store& s;
		std::string db;
		~Drop() {
			try {
				s.execute("DROP DATABASE IF EXISTS " + storage::clickhouse::quote_ident(db));
			} catch (...) {
			}
		}
	} drop{s, o.database};
	for (const auto& stmt : storage::clickhouse::schema_statements(o)) {
		if (stmt.starts_with("CREATE")) {
			s.execute(stmt);
		}
	}
	for (int i = 0; i < 2; i++) {
		ASSERT_NO_THROW(s.migrate()) << "migrate run " << i;
	}
	EXPECT_NO_THROW(s.ping(2s));
	auto now = etrace::now();
	model::PodRef pod{"u1", "api-0", "shop", "app"};
	auto tc = *model::parse_trace_context("4bf92f3577b34da6a3ce929d0e0e4736", "00f067aa0ba902b7");
	model::Batch b;
	b.id = "b1";
	b.resource = model::Resource{"n1", "a", "", {{"cluster", "c1"}}};
	b.window_start = now - 1s;
	b.window_end = now;
	b.offcpu = {model::OffCPUSample{model::Task{1, 1, "api", "", pod},
	                                testing::frames({"futex_wait"}),
	                                {model::Hop{model::Task{2, 2, "holder", "", {}}, testing::frames({"unlock"}),
	                                            testing::frames({"read"}), 4, tc}},
	                                5,
	                                1,
	                                model::OffCPUReason::blocked}};
	b.events = {model::OffCPUEvent{
	        model::Task{1, 2, "api", "", pod},
	        testing::frames({"futex_wait"}),
	        {model::Hop{model::Task{2, 2, "holder", "", {}}, testing::frames({"unlock"}), {}, 0, tc}},
	        now - 1s,
	        now,
	        model::OffCPUReason::blocked,
	        tc}};
	s.write_batch(b);
	s.write_batch(b);
	storage::Filter hit;
	hit.from = now - 1min;
	hit.to = now + 1min;
	auto miss = hit;
	hit.ns = "shop";
	hit.pod = "api-0";
	hit.labels = {{"cluster", "c1"}};
	miss.ns = "other";
	storage::ChainQuery cq;
	cq.filter = hit;
	cq.depth = 4;
	auto chains = s.offcpu_chains(cq);
	ASSERT_EQ(chains.size(), 1u);
	EXPECT_EQ(chains[0].total_ns, 5u) << "the repeated insert must be deduplicated";
	ASSERT_EQ(chains[0].chain.size(), 1u);
	EXPECT_EQ(chains[0].chain[0].wait_stack, std::vector<std::string>{"read"});
	EXPECT_EQ(chains[0].chain[0].wait_ns, 4u);
	storage::EventQuery eq;
	eq.filter = hit;
	eq.trace_id = "4bf92f3577b34da6a3ce929d0e0e4736";
	auto events = s.events(eq);
	ASSERT_EQ(events.size(), 1u);
	EXPECT_EQ(events[0].pod, "api-0");
	EXPECT_EQ(events[0].container, "app");
	EXPECT_EQ(events[0].start, b.events[0].start);
	ASSERT_EQ(events[0].chain.size(), 1u);
	EXPECT_EQ(events[0].chain[0].span_id, "00f067aa0ba902b7");
	eq.filter = miss;
	EXPECT_TRUE(s.events(eq).empty()) << "namespace filter ignored";

	model::Batch later{
	        .id = "later-wait", .resource = b.resource, .window_start = now - 500ms, .window_end = now + 1s};
	later.events = {{.task = model::Task{1, 2, "api", "", pod},
	                 .stack = testing::frames({"disk_read"}),
	                 .start = later.window_start,
	                 .end = later.window_end,
	                 .reason = model::OffCPUReason::blocked,
	                 .trace = tc}};
	s.write_batch(later);
	eq.filter = hit;
	eq.limit = 1;
	EXPECT_EQ(s.events(eq).size(), 1u);
	const auto report = query::Service(s).trace_report(nullptr, eq.trace_id, hit, eq.limit);
	EXPECT_EQ(report.offcpu_total_ns, 2'500'000'000u);
	ASSERT_EQ(report.unattributed.size(), 1u);
	EXPECT_EQ(report.unattributed[0].start, b.events[0].start);
	ASSERT_EQ(report.causes.size(), 2u);
	EXPECT_EQ(report.causes[0].root, "disk_read");

	auto observed = b.offcpu.front();
	observed.task.comm = "cache";
	observed.chain.front().wait_stack.clear();
	observed.chain.front().wait_ns = 0;
	auto inferred = observed;
	inferred.blocked_ns = 10;
	inferred.chain.front().wait_ns = 4;
	b.id = "wake-evidence";
	b.offcpu = {observed, inferred};
	b.events.clear();
	s.write_batch(b);
	cq.filter.comm = "cache";
	chains = s.offcpu_chains(cq);
	ASSERT_EQ(chains.size(), 2u) << "missing wait stacks must not combine observed and inferred samples";
	EXPECT_EQ(chains[0].total_ns, 10u);
	EXPECT_EQ(chains[0].chain.at(0).wait_ns, 4u);
	EXPECT_EQ(chains[1].total_ns, 5u);
	EXPECT_EQ(chains[1].chain.at(0).wait_ns, 0u);
	EXPECT_NO_THROW(s.ping(2s));
}

}  // namespace
}  // namespace etrace
