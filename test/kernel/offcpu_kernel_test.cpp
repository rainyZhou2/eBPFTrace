#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <format>
#include <set>
#include <sstream>
#include <thread>

#include "agent/source/offcpu/source.h"
#include "symbol/resolver.h"

// These tests load the BPF programs, so they need root, kernel BTF and the
// host PID namespace (thread ids in events are host ids). Scenario binaries
// are passed through ETRACE_SCENARIO_* variables.

namespace etrace {
namespace {

using namespace agent::offcpu;

#define REQUIRE_BPF()                                              \
	if (geteuid() != 0) {                                      \
		GTEST_SKIP() << "requires root";                   \
	}                                                          \
	if (!std::filesystem::exists("/sys/kernel/btf/vmlinux")) { \
		GTEST_SKIP() << "requires kernel BTF";             \
	}

// Child runs a scenario binary with pipes on stdin and stdout.
class Child {
public:
	explicit Child(const std::vector<std::string>& argv) {
		int in[2], out[2];
		if (pipe(in) != 0 || pipe(out) != 0) {
			throw Error("pipe failed");
		}
		pid_ = fork();
		if (pid_ == 0) {
			dup2(in[0], 0);
			dup2(out[1], 1);
			close(in[1]);
			close(out[0]);
			std::vector<char*> args;
			for (const auto& a : argv) {
				args.push_back(const_cast<char*>(a.c_str()));
			}
			args.push_back(nullptr);
			execv(args[0], args.data());
			_exit(127);
		}
		close(in[0]);
		close(out[1]);
		stdin_ = in[1];
		out_ = fdopen(out[0], "r");
	}
	~Child() {
		if (pid_ > 0 && !waited_) {
			kill(pid_, SIGKILL);
			waitpid(pid_, nullptr, 0);
		}
		if (stdin_ >= 0) {
			close(stdin_);
		}
		if (out_) {
			fclose(out_);
		}
	}
	uint32_t pid() const { return static_cast<uint32_t>(pid_); }
	void send(char c) { ASSERT_EQ(write(stdin_, &c, 1), 1); }
	std::string line() {
		char buf[256];
		return fgets(buf, sizeof buf, out_) ? std::string(buf) : "";
	}
	// wait returns the exit status and everything printed.
	std::pair<int, std::string> wait() {
		std::string all;
		for (std::string l; !(l = line()).empty();) {
			all += l;
		}
		int st = 0;
		waitpid(pid_, &st, 0);
		waited_ = true;
		return {st, all};
	}

private:
	pid_t pid_ = -1;
	int stdin_ = -1;
	FILE* out_ = nullptr;
	bool waited_ = false;
};

struct Harness {
	telemetry::Registry reg{false};
	std::unique_ptr<Source> src;

	explicit Harness(Options o) {
		o.validate();
		src = std::make_unique<Source>(o, agent::Deps{log::Logger::discard(), reg, "/proc"});
		src->start();
	}
	model::Batch collect() {
		model::Batch b;
		src->collect(b);
		return b;
	}
};

std::string env(const char* name) {
	const char* v = std::getenv(name);
	return v ? v : "";
}

bool is_chain(const std::vector<model::Hop>& c, uint32_t holder, uint32_t sleeper) {
	return c.size() >= 2 && c[0].task.tid == holder && c[0].waited() &&
	       c[0].wait_ns > static_cast<uint64_t>(Duration(20ms).count()) && c[1].task.tid == sleeper;
}

// The lockchain scenario runs holder, waiter and sleeper threads: the
// holder takes a mutex and blocks on a pipe the sleeper writes after
// sleeping; the waiter blocks on the mutex. Each round prints the "waiter
// holder sleeper" tids.
TEST(Kernel, MultiHopChain) {
	REQUIRE_BPF();
	auto bin = env("ETRACE_SCENARIO_LOCKCHAIN");
	if (bin.empty()) {
		GTEST_SKIP() << "ETRACE_SCENARIO_LOCKCHAIN not set";
	}
	Child c({bin, "5"});
	Options o;
	o.target_pid = c.pid();
	o.trace_context.enabled = false;
	o.slow_event = 20ms;
	Harness h(o);
	c.send('g');
	auto [st, out] = c.wait();
	ASSERT_EQ(st, 0) << out;
	auto b = h.collect();

	std::istringstream rounds(out);
	int total = 0, matched = 0;
	for (std::string line; std::getline(rounds, line);) {
		uint32_t w, hl, sl;
		ASSERT_EQ(std::sscanf(line.c_str(), "%u %u %u", &w, &hl, &sl), 3) << line;
		total++;
		for (const auto& e : b.events) {
			if (e.task.tid == w && is_chain(e.chain, hl, sl)) {
				matched++;
				break;
			}
		}
	}
	EXPECT_EQ(matched, total) << "scenario output:\n" << out << "events: " << b.events.size();
	bool aggregated = false;
	for (const auto& s : b.offcpu) {
		aggregated = aggregated || (s.chain.size() >= 2 && s.chain[0].waited() &&
		                            s.chain[0].wait_ns > static_cast<uint64_t>(Duration(20ms).count()));
	}
	EXPECT_TRUE(aggregated) << "aggregated samples carry no multi-hop chain";
}

// The traced scenario publishes a trace context through libetrace_ctx,
// blocks about 40ms on a pipe, clears the context and blocks again. Only
// the first long wait may carry the trace id.
TEST(Kernel, TraceContextAttribution) {
	REQUIRE_BPF();
	auto bin = env("ETRACE_SCENARIO_TRACED");
	if (bin.empty()) {
		GTEST_SKIP() << "ETRACE_SCENARIO_TRACED not set";
	}
	const std::string trace_id = "4bf92f3577b34da6a3ce929d0e0e4736", span_id = "00f067aa0ba902b7";
	Child c({bin, trace_id, span_id});
	Options o;
	o.target_pid = c.pid();
	o.trace_context.scan_interval = 1s;
	o.slow_event = 20ms;
	Harness h(o);
	auto deadline = std::chrono::steady_clock::now() + 5s;
	while (h.src->probes_attached() == 0 && std::chrono::steady_clock::now() < deadline) {
		h.src->sync_probes();
		std::this_thread::sleep_for(50ms);
	}
	ASSERT_NE(h.src->probes_attached(), 0u) << "trace context uprobe was not attached";
	c.send('g');
	auto [st, out] = c.wait();
	ASSERT_EQ(st, 0) << out;
	auto b = h.collect();
	uint32_t tid = static_cast<uint32_t>(std::stoul(out));
	int traced = 0, untraced = 0;
	for (const auto& e : b.events) {
		if (e.task.tid != tid || e.duration() < 30ms) {
			continue;
		}
		if (e.trace.trace_id_hex() == trace_id && e.trace.span_id_hex() == span_id) {
			traced++;
		} else if (!e.trace.valid()) {
			untraced++;
		} else {
			ADD_FAILURE() << "unexpected trace context " << e.trace.trace_id_hex();
		}
	}
	EXPECT_EQ(traced, 1);
	EXPECT_EQ(untraced, 1);
}

TEST(Kernel, SlowEventsEmitted) {
	REQUIRE_BPF();
	Options o;
	o.target_pid = static_cast<uint32_t>(getpid());
	o.slow_event = 20ms;
	o.trace_context.enabled = false;
	Harness h(o);
	std::this_thread::sleep_for(50ms);
	std::this_thread::sleep_for(100ms);
	auto b = h.collect();
	bool found = false;
	for (const auto& e : b.events) {
		found = found || (e.duration() >= 20ms && e.end > e.start && etrace::now() - e.end < 1min);
	}
	EXPECT_TRUE(found) << "no slow event among " << b.events.size() << " events";
}

TEST(Kernel, BlockedListsWaitsInProgress) {
	REQUIRE_BPF();
	Options o;
	o.target_pid = static_cast<uint32_t>(getpid());
	o.trace_context.enabled = false;
	Harness h(o);
	std::thread sleeper([] { std::this_thread::sleep_for(300ms); });
	std::this_thread::sleep_for(100ms);
	auto blocked = h.src->blocked();
	sleeper.join();
	bool found = false;
	for (const auto& e : blocked) {
		found = found || (e.task.pid == static_cast<uint32_t>(getpid()) && e.duration() >= 50ms);
	}
	EXPECT_TRUE(found) << blocked.size() << " blocked threads";
}

bool is_subsequence(const std::vector<std::string>& want, const std::vector<std::string>& got) {
	size_t i = 0;
	for (const auto& g : got) {
		if (i < want.size() && g == want[i]) {
			i++;
		}
	}
	return i == want.size();
}

std::vector<std::string> blocked_user_stack(const std::string& bin, const std::string& mode) {
	Child c({bin});
	Options o;
	o.target_pid = c.pid();
	o.trace_context.enabled = false;
	o.slow_event = 20ms;
	o.unwind.mode = mode;
	Harness h(o);
	if (mode == kUnwindDwarf) {
		auto deadline = std::chrono::steady_clock::now() + 5s;
		while (!h.src->unwind_loaded(c.pid())) {
			if (std::chrono::steady_clock::now() > deadline) {
				ADD_FAILURE() << "unwind tables of the scenario were not loaded";
				return {};
			}
			std::this_thread::sleep_for(20ms);
		}
	}
	c.send('g');
	auto tid = static_cast<uint32_t>(std::stoul(c.line()));
	auto b = h.collect();
	symbol::Config rc;
	rc.process_cache = 8;
	rc.elf_cache = 8;
	symbol::ProcResolver r(rc);
	for (const auto& e : b.events) {
		if (e.task.tid != tid || e.duration() < 30ms) {
			continue;
		}
		std::vector<std::string> names;
		for (const auto& f : e.stack) {
			if (f.kind == model::FrameKind::user) {
				names.push_back(r.user(c.pid(), f.address).function);
			}
		}
		c.send('q');
		return names;
	}
	ADD_FAILURE() << "no blocked event for tid " << tid << " among " << b.events.size() << " events";
	return {};
}

// The nofp scenario blocks in level3 <- level2 <- level1 <- main, built
// with -fomit-frame-pointer. The DWARF walker must recover the whole chain;
// the frame pointer result is printed for comparison only.
TEST(Kernel, UserStackWithoutFramePointers) {
	REQUIRE_BPF();
	auto bin = env("ETRACE_SCENARIO_NOFP");
	if (bin.empty()) {
		GTEST_SKIP() << "ETRACE_SCENARIO_NOFP not set";
	}
	const std::vector<std::string> want{"level3", "level2", "level1", "main"};
	for (const auto* mode : {kUnwindDwarf, kUnwindFramePointer}) {
		auto got = blocked_user_stack(bin, mode);
		std::string joined;
		for (const auto& g : got) {
			joined += g + " ";
		}
		std::printf("%s user stack: %s\n", mode, joined.c_str());
		if (std::string(mode) == kUnwindDwarf) {
			EXPECT_TRUE(is_subsequence(want, got)) << "user stack " << joined;
		}
	}
}

// describe_gpu_wait renders a GPU wait as "call kernel [nccl] stream launches".
std::string describe_gpu_wait(const model::OffCPUEvent& e, symbol::ProcResolver& r) {
	std::string call, kernel, nccl;
	for (const auto& f : e.stack) {
		if (f.kind == model::FrameKind::gpu_kernel) {
			kernel = !f.function.empty() ? f.function : r.user(e.task.pid, f.address).function;
		} else if (f.module == "[gpu]" && f.function.starts_with("waiting on ")) {
			nccl = f.function.substr(11) + " ";
		} else if (f.module == "[gpu]") {
			call = f.function;
		}
	}
	return std::format("{} {} {}{:#x} {}", call, kernel, nccl, e.gpu.stream, e.gpu.launches);
}

// The gpu scenario links stand-ins for libcuda, libcudart and libnccl and
// makes four GPU waits (see test/scenario/gpu.c). Each must come out once,
// named by its CUDA call and the kernel queued last on what it waited for.
void gpu_waits(const std::string& bin, const char* mode) {
	SCOPED_TRACE(mode);
	Child c({bin});
	uint32_t tid = 0;
	unsigned long long stub = 0;
	ASSERT_EQ(std::sscanf(c.line().c_str(), "%u %llx", &tid, &stub), 2);
	Options o;
	o.target_pid = c.pid();
	o.trace_context.enabled = false;
	o.slow_event = 20ms;
	o.unwind.mode = mode;
	o.gpu.scan_interval = 1s;
	Harness h(o);
	auto deadline = std::chrono::steady_clock::now() + 5s;
	while ((h.src->gpu_libraries_probed() < 3 || h.src->gpu_launch_libraries_probed() < 2) &&
	       std::chrono::steady_clock::now() < deadline) {
		h.src->sync_gpu_probes();
		std::this_thread::sleep_for(50ms);
	}
	ASSERT_EQ(h.src->gpu_libraries_probed(), 3u) << "GPU wait probes were not attached to the three libraries";
	ASSERT_EQ(h.src->gpu_launch_libraries_probed(), 2u)
	        << "launch probes were not attached to libcuda and libcudart";
	c.send('g');
	ASSERT_EQ(c.line(), "done\n");
	auto b = h.collect();
	symbol::Config rc;
	rc.process_cache = 8;
	rc.elf_cache = 8;
	symbol::ProcResolver r(rc);
	ASSERT_EQ(r.user(c.pid(), stub).function, "matmul_kernel");

	std::multiset<std::string> got;
	int plain_long = 0;
	for (const auto& e : b.events) {
		if (e.task.tid != tid || e.duration() < 30ms) {
			continue;
		}
		if (e.reason != model::OffCPUReason::gpu) {
			plain_long++;
			continue;
		}
		auto d = describe_gpu_wait(e, r);
		got.insert(d);
		bool spins = d.starts_with("cuCtxSynchronize");
		EXPECT_EQ(e.chain.empty(), spins) << d << ": only the sleeping waits have a wakeup chain";
		EXPECT_TRUE(std::any_of(e.stack.begin(), e.stack.end(),
		                        [](const auto& f) { return f.kind == model::FrameKind::user; }))
		        << d << ": the user stack of the call is missing";
	}
	c.send('q');
	auto [st, out] = c.wait();
	ASSERT_EQ(st, 0) << out;
	std::multiset<std::string> want{
	        "cuStreamSynchronize flash_attn_fwd 0x5000 1",
	        "cudaEventSynchronize matmul_kernel 0x6000 3",
	        "cudaStreamSynchronize matmul_kernel ncclAllReduce 0x0 4",
	        "cuCtxSynchronize matmul_kernel 0x0 0",
	};
	EXPECT_EQ(got, want);
	EXPECT_EQ(plain_long, 0) << "sleeps inside GPU waits must not be counted again as off-CPU time";
	bool aggregated = false;
	for (const auto& s : b.offcpu) {
		aggregated = aggregated || (s.task.tid == tid && s.reason == model::OffCPUReason::gpu &&
		                            s.blocked_ns >= static_cast<uint64_t>(Duration(30ms).count()));
	}
	EXPECT_TRUE(aggregated) << "aggregated samples carry no GPU wait";
}

TEST(Kernel, GpuWaits) {
	REQUIRE_BPF();
	auto bin = env("ETRACE_SCENARIO_GPU");
	if (bin.empty()) {
		GTEST_SKIP() << "ETRACE_SCENARIO_GPU not set";
	}
	for (const auto* mode : {kUnwindDwarf, kUnwindFramePointer}) {
		gpu_waits(bin, mode);
	}
}

// A runtime graph launch calls the driver itself. Only the runtime call may
// be counted; the nested driver launch must not add a second graph to the
// stream. A direct driver graph launch still counts once. The test uses the
// auto unwinder so it also runs on kernels without bpf_loop.
TEST(Kernel, GpuGraphLaunches) {
	REQUIRE_BPF();
	auto bin = env("ETRACE_SCENARIO_GPU");
	if (bin.empty()) {
		GTEST_SKIP() << "ETRACE_SCENARIO_GPU not set";
	}
	Child c({bin, "graph"});
	uint32_t tid = 0;
	unsigned long long stub = 0;
	ASSERT_EQ(std::sscanf(c.line().c_str(), "%u %llx", &tid, &stub), 2);
	(void)stub;

	Options o;
	o.target_pid = c.pid();
	o.trace_context.enabled = false;
	o.slow_event = 20ms;
	o.gpu.scan_interval = 1s;
	Harness h(o);
	auto deadline = std::chrono::steady_clock::now() + 5s;
	while ((h.src->gpu_libraries_probed() < 3 || h.src->gpu_launch_libraries_probed() < 2) &&
	       std::chrono::steady_clock::now() < deadline) {
		h.src->sync_gpu_probes();
		std::this_thread::sleep_for(50ms);
	}
	ASSERT_EQ(h.src->gpu_libraries_probed(), 3u) << "GPU wait probes were not attached";
	ASSERT_EQ(h.src->gpu_launch_libraries_probed(), 2u) << "launch probes were not attached";
	c.send('g');
	ASSERT_EQ(c.line(), "done\n");
	auto b = h.collect();

	symbol::Config rc;
	rc.process_cache = 8;
	rc.elf_cache = 8;
	symbol::ProcResolver r(rc);
	std::multiset<std::string> got;
	int plain_long = 0;
	for (const auto& e : b.events) {
		if (e.task.tid != tid || e.duration() < 30ms) {
			continue;
		}
		if (e.reason != model::OffCPUReason::gpu) {
			plain_long++;
			continue;
		}
		got.insert(describe_gpu_wait(e, r));
	}
	c.send('q');
	auto [st, out] = c.wait();
	ASSERT_EQ(st, 0) << out;
	std::multiset<std::string> want{
	        "cudaStreamSynchronize CUDA graph 0x7000 1",
	        "cuStreamSynchronize CUDA graph 0x7100 1",
	};
	EXPECT_EQ(got, want);
	EXPECT_EQ(plain_long, 0) << "sleeps inside GPU waits must not be counted again as off-CPU time";
}

// Launch probes trap on every launch, so they are detached once the launch
// rate exceeds the limit; the waits themselves stay recorded.
TEST(Kernel, GpuLaunchTrackingStopsAboveRate) {
	REQUIRE_BPF();
	auto bin = env("ETRACE_SCENARIO_GPU");
	if (bin.empty()) {
		GTEST_SKIP() << "ETRACE_SCENARIO_GPU not set";
	}
	Child c({bin});
	c.line();
	Options o;
	o.target_pid = c.pid();
	o.trace_context.enabled = false;
	o.gpu.max_launch_rate = 1e-6;
	Harness h(o);
	auto deadline = std::chrono::steady_clock::now() + 5s;
	while (h.src->gpu_launch_libraries_probed() < 2 && std::chrono::steady_clock::now() < deadline) {
		h.src->sync_gpu_probes();
		std::this_thread::sleep_for(50ms);
	}
	ASSERT_EQ(h.src->gpu_launch_libraries_probed(), 2u);
	c.send('g');
	ASSERT_EQ(c.line(), "done\n");
	h.src->sync_gpu_probes();
	EXPECT_EQ(h.src->gpu_launch_libraries_probed(), 0u) << "launch probes must be detached";
	EXPECT_EQ(h.src->gpu_libraries_probed(), 3u) << "wait probes must stay attached";
	c.send('q');
	EXPECT_EQ(c.wait().first, 0);
}

// A GPU wait that has not returned is listed as blocked whether the thread
// spins or sleeps in it, once, named by the kernel it waits for.
TEST(Kernel, BlockedListsGpuWaitsInProgress) {
	REQUIRE_BPF();
	auto bin = env("ETRACE_SCENARIO_GPU");
	if (bin.empty()) {
		GTEST_SKIP() << "ETRACE_SCENARIO_GPU not set";
	}
	setenv("FAKE_CUDA_MS", "1000", 1);
	Child c({bin, "hang"});
	unsetenv("FAKE_CUDA_MS");
	uint32_t tid = 0;
	ASSERT_EQ(std::sscanf(c.line().c_str(), "%u", &tid), 1);
	Options o;
	o.target_pid = c.pid();
	o.trace_context.enabled = false;
	Harness h(o);
	auto deadline = std::chrono::steady_clock::now() + 5s;
	while ((h.src->gpu_libraries_probed() < 3 || h.src->gpu_launch_libraries_probed() < 2) &&
	       std::chrono::steady_clock::now() < deadline) {
		h.src->sync_gpu_probes();
		std::this_thread::sleep_for(50ms);
	}
	ASSERT_EQ(h.src->gpu_launch_libraries_probed(), 2u);
	c.send('g');
	for (const char* phase : {"spin\n", "sleep\n"}) {
		SCOPED_TRACE(phase);
		ASSERT_EQ(c.line(), phase);
		std::this_thread::sleep_for(300ms);
		std::vector<std::string> got;
		for (const auto& e : h.src->blocked()) {
			if (e.task.tid != tid) {
				continue;
			}
			std::string d = model::to_string(e.reason);
			for (const auto& f : e.stack) {
				if (f.module == "[gpu]" || f.module == model::kGpuKernelModule) {
					d += " " + f.function;
				}
			}
			got.push_back(std::format("{} {:#x} {}", d, e.gpu.stream, e.duration() >= 200ms));
		}
		std::string call =
		        std::string(phase) == "spin\n" ? "cuCtxSynchronize 0x0" : "cuStreamSynchronize 0x5000";
		EXPECT_EQ(got, std::vector<std::string>{"gpu stuck_kernel " + call + " true"});
	}
	EXPECT_EQ(c.wait().first, 0);
}

TEST(Kernel, ZeroStackSamplingKeepsCollecting) {
	REQUIRE_BPF();
	Options o;
	o.target_pid = getpid();
	o.trace_context.enabled = false;
	o.chain_depth = 0;
	o.sample_percent = 0;
	Harness h(o);
	EXPECT_NO_THROW(h.collect());
	std::this_thread::sleep_for(100ms);
	EXPECT_NO_THROW(h.collect());
}

TEST(Kernel, AdaptivePolicyRunsWhileCollecting) {
	REQUIRE_BPF();
	Options o;
	o.target_pid = getpid();
	o.trace_context.enabled = false;
	o.policy.enabled = true;
	o.policy.interval = 100ms;
	o.policy.cooldown = 0s;
	o.policy.max_cpu_ratio = 0;
	o.policy.max_drop_rate = 1e9;
	o.policy.max_processes = 0;
	o.policy.allow_mode_switch = true;
	Harness h(o);
	EXPECT_NO_THROW(h.collect());
	std::this_thread::sleep_for(400ms);
	EXPECT_EQ(h.src->unwind_mode(), "frame_pointer");
	EXPECT_NO_THROW(h.collect());
}

}  // namespace
}  // namespace etrace
