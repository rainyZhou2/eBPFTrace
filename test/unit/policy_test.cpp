#include <gtest/gtest.h>

#include "agent/policy.h"
#include "platform/config.h"
#include "platform/error.h"

namespace etrace {
namespace {

agent::policy::Config config() {
	agent::policy::Config c;
	c.enabled = true;
	c.interval = 1s;
	c.cooldown = 0s;
	c.max_events_per_interval = 20000;
	c.min_events_per_interval = 1000;
	c.max_unwind_depth = 64;
	c.min_unwind_depth = 16;
	c.min_sample_percent = 10;
	c.target_event_rate = 1000;
	c.max_cpu_ratio = 0.25;
	c.max_drop_rate = 0;
	c.max_processes = 100;
	return c;
}

agent::policy::Decision decision() {
	return agent::policy::Decision{10000, 64, 100, "dwarf"};
}

TEST(Policy, ReducesLimitsUnderCpuPressure) {
	agent::policy::Planner planner(config());
	agent::policy::Signals previous;
	agent::policy::Signals current;
	current.cpu_seconds = 1;
	current.dwarf_available = true;
	auto next = planner.decide(previous, current, 1s, decision());
	EXPECT_EQ(next.max_events_per_interval, 7500u);
	EXPECT_EQ(next.unwind_depth, 48u);
	EXPECT_EQ(next.sample_percent, 75u);
	EXPECT_EQ(next.unwind_mode, "frame_pointer");
}

TEST(Policy, ReducesLimitsUnderDropAndProcessPressure) {
	agent::policy::Planner planner(config());
	agent::policy::Signals previous;
	agent::policy::Signals current;
	current.bpf_drops = 1;
	current.processes = 101;
	current.dwarf_available = true;
	auto next = planner.decide(previous, current, 1s, decision());
	EXPECT_EQ(next.max_events_per_interval, 7500u);
	EXPECT_EQ(next.unwind_depth, 48u);
	EXPECT_EQ(next.sample_percent, 75u);
}

TEST(Policy, ReadsBpfAndQueuePressure) {
	agent::policy::Planner planner(config());
	agent::policy::Signals previous;
	agent::policy::Signals current;

	auto bpf = current;
	bpf.bpf_run_time_ns = 1'000'000'000;
	EXPECT_EQ(planner.decide(previous, bpf, 1s, decision()).sample_percent, 75u);

	auto ring = current;
	ring.ringbuf_drops = 1;
	EXPECT_EQ(planner.decide(previous, ring, 1s, decision()).sample_percent, 75u);

	auto queue = current;
	queue.queue_saturation = 1;
	EXPECT_EQ(planner.decide(previous, queue, 1s, decision()).sample_percent, 75u);

	auto capacity = current;
	capacity.capacity_drops = 1;
	EXPECT_EQ(planner.decide(previous, capacity, 1s, decision()).sample_percent, 75u);
}

TEST(Policy, IgnoresAcceptedRateAndActiveSamplingDrops) {
	agent::policy::Planner planner(config());
	agent::policy::Signals previous;
	agent::policy::Signals current;
	current.accepted_events = 100'000;
	current.sampled_waits = 100'000;
	current.rejected_events = 100'000;
	auto steady = agent::policy::Decision{20000, 64, 100, "dwarf"};
	auto next = planner.decide(previous, current, 1s, steady);
	EXPECT_EQ(next.max_events_per_interval, steady.max_events_per_interval);
	EXPECT_EQ(next.unwind_depth, steady.unwind_depth);
	EXPECT_EQ(next.sample_percent, steady.sample_percent);
	EXPECT_EQ(next.unwind_mode, steady.unwind_mode);
}

TEST(Policy, RaisesLimitsWhenThereIsHeadroom) {
	agent::policy::Planner planner(config());
	agent::policy::Signals previous;
	agent::policy::Signals current;
	current.raw_waits = 100;
	previous.raw_waits = 0;
	current.dwarf_available = true;
	auto low = decision();
	low.max_events_per_interval = 10000;
	low.unwind_depth = 32;
	low.sample_percent = 50;
	low.unwind_mode = "frame_pointer";
	auto next = planner.decide(previous, current, 1s, low);
	EXPECT_EQ(next.max_events_per_interval, 15000u);
	EXPECT_EQ(next.unwind_depth, 40u);
	EXPECT_EQ(next.sample_percent, 60u);
	EXPECT_EQ(next.unwind_mode, "dwarf");
}

TEST(Policy, DoesNotSwitchModeWithoutDwarfSupport) {
	agent::policy::Planner planner(config());
	agent::policy::Signals previous;
	agent::policy::Signals current;
	current.cpu_seconds = 1;
	auto next = planner.decide(previous, current, 1s, decision());
	EXPECT_EQ(next.unwind_mode, "dwarf");
}

TEST(Policy, DecodesAndValidatesConfig) {
	auto node = YAML::Load(R"(enabled: true
interval: 5s
cooldown: 20s
max_events_per_interval: 12000
target_event_rate: 2500
max_cpu_ratio: 0.4
)");
	agent::policy::Config c;
	config::decode(node, c, "policy");
	EXPECT_TRUE(c.enabled);
	EXPECT_EQ(c.interval, 5s);
	EXPECT_EQ(c.cooldown, 20s);
	EXPECT_EQ(c.max_events_per_interval, 12000u);
	EXPECT_DOUBLE_EQ(c.target_event_rate, 2500);
	EXPECT_DOUBLE_EQ(c.max_cpu_ratio, 0.4);
}

TEST(Policy, RejectsInvalidBounds) {
	auto c = config();
	c.min_events_per_interval = 0;
	EXPECT_THROW(c.validate(), Error);
	c = config();
	c.max_unwind_depth = 65;
	EXPECT_THROW(c.validate(), Error);
	c = config();
	c.min_sample_percent = 90;
	c.max_sample_percent = 80;
	EXPECT_THROW(c.validate(), Error);
}

}  // namespace
}  // namespace etrace
