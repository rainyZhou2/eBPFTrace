#include <gtest/gtest.h>

#include <sstream>

#include "doctor/report.h"
#include "query/service.h"

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
