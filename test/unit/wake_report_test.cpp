#include <gtest/gtest.h>

#include <sstream>

#include "doctor/report.h"
#include "query/service.h"
#include "storage/memory/memory.h"

namespace etrace {
namespace {

storage::Event overlapping_wait() {
	return storage::Event{.comm = "load_cache",
	                      .stack = {"futex_wait"},
	                      .chain = {{"cache_holder", 20, {"unlock"}, {"poll"}, 40, {}, {}},
	                                {"decoy", 30, {"signal_pipe"}, {}, 0, {}, {}}},
	                      .end = from_unix_nano(100),
	                      .reason = "blocked"};
}

TEST(WakeReport, EventIdentifiesOnlyTheDirectWakeupAsObserved) {
	auto j = query::to_json(overlapping_wait());
	const auto& chain = j.at("chain");
	ASSERT_EQ(chain.size(), 2u);
	EXPECT_EQ(chain.at(0).at("wake_evidence"), "observed");
	EXPECT_EQ(chain.at(0).at("wait_evidence"), "inferred");
	EXPECT_EQ(chain.at(1).at("wake_evidence"), "inferred");
	EXPECT_FALSE(chain.at(1).contains("wait_evidence"));
}

TEST(WakeReport, KeepsUpstreamEvidenceWhenTheWaitStackIsMissing) {
	auto event = overlapping_wait();
	event.chain.front().wait_stack.clear();
	const auto hop = query::to_json(event).at("chain").at(0);
	EXPECT_EQ(hop.at("wait_evidence"), "inferred");
	EXPECT_FALSE(hop.contains("wait_stack"));
	auto causes = query::collapse_causes({{event.comm, event.stack, event.chain, 100, 1}}, 0);
	ASSERT_EQ(causes.size(), 1u);
	auto j = query::to_json(causes.front());
	EXPECT_EQ(j.at("path"), (query::Json{"load_cache", "cache_holder", "decoy"}));
	EXPECT_EQ(j.at("cause_evidence"), "inferred_upstream");
}

TEST(WakeReport, SeparatesObservedWakeSitesFromOverlappingWaitSites) {
	const storage::ChainValue wake{
	        "load_cache", {"futex_wait"}, {{"holder", 20, {"same_frame"}, {}, 0, {}, {}}}, 100, 1};
	auto wait = wake;
	wait.chain.front().wait_stack = {"same_frame"};
	wait.chain.front().wait_ns = 40;
	auto causes = query::to_json(query::collapse_causes({wake, wait}, 0));
	ASSERT_EQ(causes.size(), 2u);
	EXPECT_EQ(causes.at(0).at("cause_evidence"), "observed_wakeup");
	EXPECT_EQ(causes.at(1).at("cause_evidence"), "inferred_upstream");
}

TEST(WakeReport, KeepsTheObservedSiteWhenOnlyAnOverlapDurationIsAvailable) {
	const storage::ChainValue wake{
	        "load_cache", {"futex_wait"}, {{"holder", 20, {"unlock"}, {}, 0, {}, {}}}, 100, 1};
	auto wait = wake;
	wait.chain.front().wait_ns = 40;
	auto causes = query::to_json(query::collapse_causes({wake, wait}, 0));
	ASSERT_EQ(causes.size(), 2u);
	EXPECT_EQ(causes.at(1).at("root_cause"), "holder: unlock");
	EXPECT_EQ(causes.at(1).at("cause_evidence"), "observed_wakeup");
	EXPECT_EQ(causes.at(1).at("hop_wait_ns"), query::Json::array({40}));
}

TEST(WakeReport, TraceLimitOnlyCapsEventDetails) {
	const std::string trace_id = "4bf92f3577b34da6a3ce929d0e0e4736";
	const std::string root_id = "a2fb4a1d1a96d312";
	const std::string child_id = "00f067aa0ba902b7";
	const auto start = from_unix(1);
	class Backend : public tracing::Backend {
	public:
		std::vector<tracing::Span> spans;
		std::vector<tracing::Span> trace(const std::string&) override { return spans; }
	} backend;
	backend.spans = {{root_id, "", "root", "", start, start + 100ms, {}},
	                 {child_id, root_id, "child", "", start + 1ms, start + 80ms, {}}};
	const auto traced = *model::parse_trace_context(trace_id, child_id);
	const auto unattributed = *model::parse_trace_context(trace_id, "ffffffffffffffff");
	model::Batch batch{.id = "trace", .window_start = start, .window_end = start + 100ms};
	batch.events = {
	        {.task = {.comm = "late"}, .start = start + 20ms, .end = start + 90ms, .trace = traced},
	        {.task = {.comm = "early"}, .start = start + 2ms, .end = start + 5ms, .trace = traced},
	        {.task = {.comm = "unknown"}, .start = start + 6ms, .end = start + 8ms, .trace = unattributed},
	};
	storage::memory::Store store({Duration(0)});
	store.write_batch(batch);
	query::Service service(store);
	const storage::Filter filter{.from = start, .to = start + 100ms};
	const auto report = service.trace_report(&backend, trace_id, filter, 2);
	EXPECT_EQ(report.offcpu_total_ns, 75'000'000u);
	ASSERT_EQ(report.spans.size(), 2u);
	EXPECT_EQ(report.spans[0].offcpu_ns, 63'000'000u);
	EXPECT_EQ(report.spans[1].self_offcpu_ns, 63'000'000u);
	ASSERT_EQ(report.spans[1].events.size(), 1u);
	EXPECT_EQ(report.spans[1].events[0].comm, "early");
	ASSERT_EQ(report.unattributed.size(), 1u);
	EXPECT_EQ(report.unattributed[0].comm, "unknown");
	ASSERT_EQ(report.causes.size(), 3u);
	EXPECT_EQ(report.causes[0].path, std::vector<std::string>{"late"});
	EXPECT_EQ(report.causes[0].total_ns, 70'000'000u);

	const auto smaller_report = service.trace_report(&backend, trace_id, filter, 1);
	EXPECT_EQ(smaller_report.offcpu_total_ns, report.offcpu_total_ns);
	ASSERT_EQ(smaller_report.spans.size(), 2u);
	EXPECT_EQ(smaller_report.spans[0].offcpu_ns, report.spans[0].offcpu_ns);
	EXPECT_EQ(smaller_report.spans[1].self_offcpu_ns, report.spans[1].self_offcpu_ns);
	EXPECT_TRUE(smaller_report.unattributed.empty());
	EXPECT_EQ(query::to_json(smaller_report.causes), query::to_json(report.causes));

	const auto without_backend = service.trace_report(nullptr, trace_id, filter, 1);
	EXPECT_EQ(without_backend.offcpu_total_ns, 75'000'000u);
	ASSERT_EQ(without_backend.unattributed.size(), 1u);
	EXPECT_EQ(without_backend.unattributed[0].comm, "early");
	EXPECT_EQ(query::to_json(without_backend.causes), query::to_json(report.causes));
}

TEST(WakeReport, DoctorMarksEachPartOfTheWaitPath) {
	auto event = overlapping_wait();
	doctor::Report report;
	report.total_ns = 100;
	report.causes = query::collapse_causes({{event.comm, event.stack, event.chain, 100, 1}}, 0);
	report.slow.push_back(event);
	std::ostringstream console;
	doctor::write_text(console, report);
	EXPECT_NE(console.str().find("cache_holder (observed) <- decoy (inferred)"), std::string::npos);
	EXPECT_NE(console.str().find("inferred upstream: decoy: signal_pipe"), std::string::npos);
	std::ostringstream html;
	doctor::write_html(html, report);
	EXPECT_NE(html.str().find("cache_holder (observed) ← decoy (inferred)"), std::string::npos);
	EXPECT_NE(html.str().find("inferred upstream: decoy: signal_pipe"), std::string::npos);
}

}  // namespace
}  // namespace etrace
