#include <gtest/gtest.h>

#include <format>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include "agent/pipeline.h"
#include "agent/processor/container.h"
#include "agent/processor/symbolize.h"
#include "agent/source/offcpu/decode.h"
#include "agent/source/offcpu/gpu.h"
#include "agent/source/offcpu/uprobes.h"
#include "testutil.h"

namespace etrace {
namespace {

class StaticSource : public agent::Source {
public:
	std::string name() const override { return "static"; }
	void start() override {}
	void collect(model::Batch& b) override {
		b.offcpu.push_back(model::OffCPUSample{model::Task{1, 0, "x", "", {}}, {}, {}, 5, 1, {}});
	}
	void close() override {}
};

class FlakyExporter : public agent::Exporter {
public:
	explicit FlakyExporter(int failures) : failures_(failures) {}
	void export_batch(std::stop_token, const model::Batch& b) override {
		std::lock_guard lock(mu_);
		if (failures_ > 0) {
			failures_--;
			throw Error("unavailable");
		}
		got_.push_back(b.id);
	}
	size_t delivered() {
		std::lock_guard lock(mu_);
		return got_.size();
	}

private:
	std::mutex mu_;
	int failures_;
	std::vector<std::string> got_;
};

TEST(Pipeline, RetriesUntilDelivered) {
	testing::TempDir dir;
	spool::Spool q(dir.path(), 0);
	auto exp = std::make_unique<FlakyExporter>(2);
	auto* e = exp.get();
	agent::PipelineConfig c;
	c.resource.node = "n";
	c.interval = 20ms;
	c.sources.push_back(std::make_unique<StaticSource>());
	c.queue = &q;
	c.exporter = std::move(exp);
	c.retry = backoff::Policy{1ms, 5ms, 2, 0};
	telemetry::Registry reg(false);
	agent::Pipeline p(std::move(c), reg);
	std::stop_source stop;
	std::thread t([&] { p.run(stop.get_token()); });
	bool ok = testing::wait_for([&] { return e->delivered() >= 2; });
	stop.request_stop();
	t.join();
	EXPECT_TRUE(ok) << "delivered " << e->delivered();
	EXPECT_GE(testing::metric(reg, "etrace_agent_batches_exported_total", "result", "retry"), 2);
	p.close();
}

TEST(Container, ParseCgroup) {
	std::string id(64, 'a');
	for (size_t i = 1; i < id.size(); i += 2) {
		id[i] = 'b';
	}
	struct Case {
		std::string in, id, pod;
	};
	std::vector<Case> cases{
	        {"0::/kubepods.slice/kubepods-burstable.slice/"
	         "kubepods-burstable-pod1b2c3d4e_0000_1111_2222_333344445555.slice/"
	         "cri-containerd-" +
	                 id + ".scope\n",
	         id, "1b2c3d4e-0000-1111-2222-333344445555"},
	        {"12:cpu,cpuacct:/kubepods/besteffort/pod1b2c3d4e-0000-1111-2222-333344445555/" + id + "\n", id,
	         "1b2c3d4e-0000-1111-2222-333344445555"},
	        {"0::/system.slice/docker-" + id + ".scope\n", id, ""},
	        {"0::/user.slice\n", "", ""},
	};
	for (const auto& c : cases) {
		std::istringstream in(c.in);
		auto [got_id, got_pod] = agent::container::parse_cgroup(in);
		EXPECT_EQ(got_id, c.id) << c.in;
		EXPECT_EQ(got_pod, c.pod) << c.in;
	}
}

TEST(Container, ParseRuntimeConfig) {
	std::vector<std::pair<std::string, model::PodRef>> cases{
	        {R"({"annotations":{"io.kubernetes.cri.sandbox-name":"api-7d9","io.kubernetes.cri.sandbox-namespace":"shop","io.kubernetes.cri.container-name":"app","io.kubernetes.cri.sandbox-uid":"u1"}})",
	         {"u1", "api-7d9", "shop", "app"}},
	        {R"({"annotations":{"io.kubernetes.pod.name":"db-0","io.kubernetes.pod.namespace":"data","io.kubernetes.container.name":"pg","io.kubernetes.pod.uid":"u2"}})",
	         {"u2", "db-0", "data", "pg"}},
	        {R"({"Config":{"Labels":{"io.kubernetes.pod.name":"web","io.kubernetes.pod.namespace":"default","io.kubernetes.container.name":"nginx"}}})",
	         {"", "web", "default", "nginx"}},
	};
	for (const auto& [raw, want] : cases) {
		auto got = agent::container::parse_runtime_config(raw);
		ASSERT_TRUE(got) << raw;
		EXPECT_EQ(*got, want) << raw;
	}
	EXPECT_FALSE(agent::container::parse_runtime_config(R"({"annotations":{"other":"x"}})"));
	EXPECT_FALSE(agent::container::parse_runtime_config("not json"));
}

TEST(Container, ProcessTagsTasksAndChains) {
	testing::TempDir root;
	std::string id(64, 'c'), other(64, 'e');
	for (size_t i = 1; i < 64; i += 2) {
		id[i] = 'd';
		other[i] = 'f';
	}
	testing::write_file(
	        root / "proc/10/cgroup",
	        "0::/kubepods.slice/kubepods-pod1b2c3d4e_0000_1111_2222_333344445555.slice/cri-containerd-" + id +
	                ".scope\n");
	testing::write_file(
	        root / "proc/11/cgroup",
	        "0::/kubepods.slice/kubepods-pod99999999_0000_1111_2222_333344445555.slice/cri-containerd-" + other +
	                ".scope\n");
	testing::write_file(
	        root / ("host/run/containerd/io.containerd.runtime.v2.task/k8s.io/" + id + "/config.json"),
	        R"({"annotations":{"io.kubernetes.cri.sandbox-name":"api","io.kubernetes.cri.sandbox-namespace":"shop","io.kubernetes.cri.container-name":"app"}})");
	agent::container::Options o;
	o.host_root = root / "host";
	agent::container::Processor p(root / "proc", o);
	model::Batch b;
	model::OffCPUSample s;
	s.task.pid = 10;
	s.chain = {model::Hop{model::Task{11, 0, "", "", {}}, {}, {}, 0, {}},
	           model::Hop{model::Task{12, 0, "", "", {}}, {}, {}, 0, {}}};
	b.offcpu.push_back(s);
	p.process(b);
	const auto& got = b.offcpu[0];
	EXPECT_EQ(got.task.container_id, id);
	EXPECT_EQ(got.task.pod, (model::PodRef{"1b2c3d4e-0000-1111-2222-333344445555", "api", "shop", "app"}));
	EXPECT_EQ(got.chain[0].task.pod.uid, "99999999-0000-1111-2222-333344445555");
	EXPECT_EQ(got.chain[0].task.pod.name, "") << "unresolvable runtime state must keep the pod UID only";
	EXPECT_EQ(got.chain[1].task.container_id, "");
	EXPECT_TRUE(got.chain[1].task.pod.empty()) << "missing process must stay untagged";
}

class FakeResolver : public symbol::Resolver {
public:
	std::map<uint64_t, std::string> names;
	symbol::Resolved kernel(uint64_t a) override { return {names[a], "[kernel]"}; }
	symbol::Resolved user(uint32_t, uint64_t a) override { return {names[a], "app"}; }
};

TEST(Symbolize, ResolveTrimsTracingFrames) {
	auto r = std::make_unique<FakeResolver>();
	r->names = {{1, "bpf_prog_abc_on_switch"},
	            {2, "bpf_trace_run4"},
	            {3, "__bpf_trace_sched_switch"},
	            {4, "__schedule"},
	            {5, "main"}};
	agent::symbolize::Processor p(std::move(r));
	auto k = [](uint64_t a) { return model::Frame{a, "", "", model::FrameKind::kernel}; };
	model::Batch b;
	model::OffCPUSample s;
	s.stack = {k(1), k(2), k(3), k(4), model::Frame{5, "", "", model::FrameKind::user}};
	s.chain = {model::Hop{{}, {k(1), k(4)}, {}, 0, {}}};
	b.offcpu.push_back(s);
	p.process(b);
	const auto& got = b.offcpu[0];
	ASSERT_EQ(got.stack.size(), 2u);
	EXPECT_EQ(got.stack[0].function, "__schedule");
	EXPECT_EQ(got.stack[1].symbol(), "app`main");
	ASSERT_EQ(got.chain[0].wake_stack.size(), 1u);
	EXPECT_EQ(got.chain[0].wake_stack[0].function, "__schedule");
}

TEST(Offcpu, DecodeEventAndChain) {
	using namespace agent::offcpu;
	RawEvent e{};
	e.start_ns = 1000;
	e.end_ns = 5000;
	e.pid = 11;
	e.tgid = 10;
	std::strcpy(e.comm, "api");
	e.kstack = 1;
	e.trace.trace_id[0] = 0xab;
	e.chain.n = 2;
	std::strcpy(e.chain.hops[0].comm, "holder");
	e.chain.hops[0].has_wait = 1;
	e.chain.hops[0].wait_ns = 300;
	e.chain.hops[0].wait_kstack = 2;
	std::strcpy(e.chain.hops[1].comm, "kworker");
	std::string raw(reinterpret_cast<const char*>(&e), sizeof e);
	auto got = decode_event(std::span(raw.data(), raw.size()));
	ASSERT_TRUE(got);
	EXPECT_FALSE(decode_event(std::span(raw.data(), raw.size() - 1)));
	std::map<uint64_t, std::vector<uint64_t>> stacks{{1, {0xa, 0xb}}, {2, {0xc}}};
	std::vector<uint64_t> none;
	StackLookup lookup = [&](uint64_t id) -> const std::vector<uint64_t>& {
		auto it = stacks.find(id);
		return it == stacks.end() ? none : it->second;
	};
	auto ev = to_event(*got, 1000000000, lookup);
	EXPECT_EQ(ev.task.comm, "api");
	EXPECT_EQ(ev.task.pid, 10u);
	EXPECT_EQ(ev.task.tid, 11u);
	EXPECT_EQ(ev.duration(), Duration(4000));
	EXPECT_EQ(unix_nano(ev.start), 1000001000);
	EXPECT_EQ(ev.stack.size(), 2u);
	EXPECT_TRUE(ev.trace.valid());
	ASSERT_EQ(ev.chain.size(), 2u);
	EXPECT_EQ(ev.chain[0].task.comm, "holder");
	EXPECT_EQ(ev.chain[0].wait_ns, 300u);
	EXPECT_EQ(ev.chain[0].wait_stack.size(), 1u);
	EXPECT_FALSE(ev.chain[1].waited());

	RawStart st{};
	st.ts = 1000;
	st.tgid = 5;
	st.kstack = 1;
	st.has_trace = 1;
	st.trace.trace_id[0] = 1;
	auto blocked = to_blocked(st, 6, "db", 4000, 1000000000, lookup);
	EXPECT_EQ(blocked.task.pid, 5u);
	EXPECT_EQ(blocked.task.tid, 6u);
	EXPECT_EQ(blocked.duration(), Duration(3000));
	EXPECT_TRUE(blocked.trace.valid());
	EXPECT_TRUE(blocked.chain.empty());

	RawChain c{};
	c.n = 1;
	c.hops[0].has_wait = 1;
	c.hops[0].wait_ns = 5;
	RawKey key{};
	key.tgid = 1;
	key.pid = 1;
	RawVal val{100, 4, {80, 0, 0, 0}};
	auto smp = to_sample(key, val, &c, lookup);
	EXPECT_EQ(smp.chain[0].wait_ns, 80u) << "aggregated hop wait must win";
	EXPECT_EQ(smp.blocked_ns, 100u);
	EXPECT_EQ(smp.count, 4u);
}

TEST(OffcpuOptions, RejectsInvalidSamplePercent) {
	agent::offcpu::Options o;
	o.sample_percent = 101;
	EXPECT_THROW(o.validate(), Error);
	o.sample_percent = -1;
	EXPECT_THROW(o.validate(), Error);
}

TEST(Uprobes, MatchMaps) {
	std::istringstream maps(R"(7f0000000000-7f0000001000 r-xp 00000000 08:01 11 /usr/lib/libetrace_ctx.so
7f0000001000-7f0000002000 r--p 00001000 08:01 11 /usr/lib/libetrace_ctx.so
7f0000003000-7f0000004000 r-xp 00000000 08:01 12 /usr/lib/libc.so.6
7f0000005000-7f0000006000 r-xp 00000000 08:01 11 /usr/lib/libetrace_ctx.so
)");
	auto got = agent::offcpu::uprobes::match_maps(maps, {"libetrace_ctx"});
	EXPECT_EQ(got, std::vector<std::string>{"/usr/lib/libetrace_ctx.so"});
}

TEST(Uprobes, ScanReportsIncompleteProc) {
	agent::offcpu::uprobes::Scanner scanner{"/nonexistent/etrace-proc", {"libetrace_ctx"}, {}};
	auto result = scanner.scan();
	EXPECT_FALSE(result.complete);
	EXPECT_TRUE(result.targets.empty());
}

TEST(Gpu, MatchSymbols) {
	using namespace agent::offcpu::gpu;
	EXPECT_EQ(api_name(match_api("cuStreamSynchronize")), "cuStreamSynchronize");
	EXPECT_EQ(api_name(match_api("cuStreamSynchronize_ptsz")), "cuStreamSynchronize");
	EXPECT_EQ(api_name(match_api("cuMemcpyDtoH_v2_ptds")), "cuMemcpyDtoH");
	EXPECT_EQ(api_name(match_api("cuMemFreeHost")), "cuMemFreeHost");
	EXPECT_EQ(api_name(match_api("cudaDeviceSynchronize")), "cudaDeviceSynchronize");
	EXPECT_EQ(match_api("cuMemcpyAsync"), 0) << "async copies do not block the host";
	EXPECT_EQ(match_api("cudaMemcpyAsync"), 0);
	EXPECT_EQ(match_api("cuStreamWaitEvent"), 0) << "the GPU waits, not the host";
	EXPECT_EQ(match_api("cuStreamSynchronizeX"), 0);
	EXPECT_EQ(nccl_name(match_nccl("ncclReduceScatter")), "ncclReduceScatter");
	EXPECT_EQ(nccl_name(match_nccl("ncclReduce")), "ncclReduce");
	EXPECT_EQ(match_nccl("ncclAllReduce_v2"), 0);
	EXPECT_EQ(api_name(0), "");
	EXPECT_EQ(api_name(200), "");
	EXPECT_EQ(match_api("cuStreamSynchronize_ptsz") >> 8, 1u) << "argument 0 is the stream";
	EXPECT_EQ(match_api("cudaEventSynchronize") >> 8, 2u) << "argument 0 is the event";
	EXPECT_EQ(match_api("cuCtxSynchronize") >> 8, 0u);
}

// The fake libraries of the kernel scenarios export the real names.
TEST(Gpu, ProbesFromDynamicSymbols) {
	using namespace agent::offcpu;
	std::string dir = std::string(ETRACE_SOURCE_DIR) + "/test/scenario/";
	std::set<std::string> enter, exit;
	for (const auto& p : gpu::probes(dir + "libcuda.so.1")) {
		(p.retprobe ? exit : enter).insert(p.symbol);
		EXPECT_NE(p.cookie, 0u) << p.symbol;
	}
	EXPECT_TRUE(enter.contains("cuStreamSynchronize"));
	EXPECT_TRUE(enter.contains("cuStreamSynchronize_ptsz"));
	EXPECT_EQ(enter, exit) << "every entry probe needs its return probe";
	EXPECT_FALSE(enter.contains("cuLaunchKernel"));

	std::map<std::string, std::string> launch;
	for (const auto& p : gpu::launch_probes(dir + "libcuda.so.1")) {
		launch[p.symbol + (p.retprobe ? " ret" : "")] = std::format("{} {}", p.prog, p.cookie);
	}
	EXPECT_EQ(launch, (std::map<std::string, std::string>{{"cuLaunchKernel", "on_gpu_launch 1"},
	                                                      {"cuGraphLaunch", "on_gpu_launch 5"},
	                                                      {"cuEventRecord", "on_gpu_event_record 0"},
	                                                      {"cuModuleGetFunction", "on_gpu_fn_enter 0"},
	                                                      {"cuModuleGetFunction ret", "on_gpu_fn_exit 0"}}));
	auto rt = gpu::launch_probes(dir + "libcudart.so.12");
	EXPECT_TRUE(std::any_of(rt.begin(), rt.end(),
	                        [](const auto& p) { return p.symbol == "cudaLaunchKernel" && p.cookie == 3; }));
	EXPECT_TRUE(std::any_of(rt.begin(), rt.end(), [](const auto& p) {
		return p.symbol == "cudaGraphLaunch" && p.cookie == 6 && !p.retprobe;
	}));
	EXPECT_TRUE(std::any_of(rt.begin(), rt.end(),
	                        [](const auto& p) { return p.symbol == "cudaGraphLaunch" && p.retprobe; }));
}

TEST(Gpu, DecodeNamesTheWait) {
	using namespace agent::offcpu;
	std::map<uint64_t, std::vector<uint64_t>> stacks{{7, {0x10, 0x20}}};
	std::vector<uint64_t> none;
	StackLookup lookup = [&](uint64_t id) -> const std::vector<uint64_t>& {
		auto it = stacks.find(id);
		return it == stacks.end() ? none : it->second;
	};
	RawKey key{};
	key.pid = 3;
	key.tgid = 3;
	key.ustack = 7;
	key.reason = kBpfReasonGpu;
	key.gpu_api = gpu::match_api("cuStreamSynchronize");
	key.gpu_op = gpu::match_nccl("ncclAllReduce");
	RawVal val{2000000, 1, {}};
	auto smp = to_sample(key, val, nullptr, lookup);
	EXPECT_EQ(smp.reason, model::OffCPUReason::gpu);
	ASSERT_EQ(smp.stack.size(), 4u);
	EXPECT_EQ(smp.stack[0].symbol(), "[gpu]`waiting on ncclAllReduce");
	EXPECT_EQ(smp.stack[1].symbol(), "[gpu]`cuStreamSynchronize");
	EXPECT_EQ(smp.stack[2].kind, model::FrameKind::user);

	RawEvent e{};
	e.tgid = 9;
	e.reason = kBpfReasonGpu;
	e.gpu_api = gpu::match_api("cudaDeviceSynchronize");
	e.gpu_kernel_kind = gpu::kKernelFunc;
	e.gpu_kernel = 0xf00d;
	e.gpu_stream = 0x5000;
	e.gpu_launches = 3;
	KernelLookup names = [](uint32_t tgid, uint64_t h) {
		return tgid == 9 && h == 0xf00d ? std::string("flash_attn_fwd") : std::string();
	};
	auto ev = to_event(e, 0, lookup, names);
	EXPECT_EQ(model::to_string(ev.reason), std::string("gpu"));
	ASSERT_EQ(ev.stack.size(), 2u);
	EXPECT_EQ(ev.stack[0].symbol(), "[gpu kernel]`flash_attn_fwd");
	EXPECT_EQ(ev.stack[1].symbol(), "[gpu]`cudaDeviceSynchronize");
	EXPECT_EQ(ev.gpu, (model::GPUWait{0x5000, 3}));
	e.gpu_kernel = 0xbad;
	EXPECT_EQ(to_event(e, 0, lookup, names).stack[0].symbol(), "[gpu kernel]`[unknown]")
	        << "handles created before the agent attached have no name";

	key.gpu_kernel_kind = gpu::kKernelStub;
	key.gpu_kernel = 0x401000;
	auto stub = to_sample(key, val, nullptr, lookup).stack;
	ASSERT_EQ(stub.size(), 5u);
	EXPECT_EQ(stub[1].kind, model::FrameKind::gpu_kernel);
	EXPECT_EQ(stub[1].address, 0x401000u) << "a runtime stub resolves like a user frame";

	key.reason = 0;
	key.gpu_kernel_kind = 0;
	EXPECT_EQ(to_sample(key, val, nullptr, lookup).stack.size(), 2u) << "off-CPU waits get no GPU frames";
}

TEST(Symbolize, ResolvesKernelStubs) {
	auto r = std::make_unique<FakeResolver>();
	r->names = {{0x401000, "_Z13matmul_kernelPfS_"}};
	agent::symbolize::Processor p(std::move(r));
	model::Batch b;
	model::OffCPUSample s;
	s.stack = {model::Frame{0x401000, "", "", model::FrameKind::gpu_kernel},
	           model::Frame{0, "flash_attn_fwd", model::kGpuKernelModule, model::FrameKind::gpu_kernel}};
	b.offcpu.push_back(s);
	p.process(b);
	EXPECT_EQ(b.offcpu[0].stack[0].symbol(), "[gpu kernel]`_Z13matmul_kernelPfS_");
	EXPECT_EQ(b.offcpu[0].stack[1].symbol(), "[gpu kernel]`flash_attn_fwd");
}

TEST(Symbolize, LabelsGpuInterrupt) {
	auto r = std::make_unique<FakeResolver>();
	r->names = {{1, "nvidia_isr_msix"}, {2, "handle_irq_event"}, {3, "e1000_intr"}};
	agent::symbolize::Processor p(std::move(r));
	auto k = [](uint64_t a) { return model::Frame{a, "", "", model::FrameKind::kernel}; };
	model::Batch b;
	model::OffCPUEvent e;
	e.chain = {model::Hop{model::Task{0, 0, "[hardirq]", "", {}}, {k(1), k(2)}, {}, 0, {}},
	           model::Hop{model::Task{0, 0, "[hardirq]", "", {}}, {k(3), k(2)}, {}, 0, {}}};
	b.events.push_back(e);
	p.process(b);
	EXPECT_EQ(b.events[0].chain[0].task.comm, "[gpu]");
	EXPECT_EQ(b.events[0].chain[1].task.comm, "[hardirq]");
}

}  // namespace
}  // namespace etrace
