#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "agent/plugin.h"
#include "agent/policy.h"
#include "agent/source/offcpu/bpf_object.h"
#include "agent/source/offcpu/decode.h"
#include "agent/source/offcpu/options.h"
#include "agent/source/offcpu/unwinder.h"
#include "agent/source/offcpu/uprobes.h"

struct ring_buffer;

namespace etrace::agent::offcpu {

// Source records off-CPU time with the eBPF program in bpf/offcpu.bpf.c:
// in-kernel aggregation into double-buffered hash maps, wakeup chains built
// at sched_waking, and individual events through a ring buffer.
class Source : public agent::Source {
public:
	Source(Options o, Deps deps);
	~Source() override;

	std::string name() const override { return "offcpu"; }
	void start() override;
	// collect flips the generation so the kernel writes into the other
	// counts map, waits for in-flight updates to finish, then drains the
	// idle map and the buffered per-interval events.
	void collect(model::Batch& b) override;
	// blocked lists threads that are off-CPU now and have been for at least
	// min_block, and threads inside a GPU wait for at least gpu.min_wait,
	// spinning or not. Such waits are not in collect output until they end,
	// so a thread hung for the whole recording would otherwise be invisible.
	// A thread sleeping inside a GPU wait is listed once, as the GPU wait.
	// Entries of threads that exited while blocked are dropped.
	std::vector<model::OffCPUEvent> blocked();
	void close() override;

	size_t probes_attached();
	void sync_probes();
	// gpu_libraries_probed returns the number of CUDA and NCCL libraries
	// with GPU wait probes attached, and gpu_launch_libraries_probed those
	// with launch tracking probes.
	size_t gpu_libraries_probed();
	size_t gpu_launch_libraries_probed();
	std::string unwind_mode() const;
	void sync_gpu_probes();
	// unwind_loaded reports whether the walker has mappings for tgid.
	bool unwind_loaded(uint32_t tgid);

private:
	std::unique_ptr<BpfObject> load_object(bool dwarf);
	std::pair<std::unique_ptr<BpfObject>, std::string> load();
	void configure(BpfObject& obj);
	void read_events(std::stop_token st);
	void start_unwinder();
	void start_gpu();
	void sync_gpu();
	uint64_t launch_hits();
	StackLookup stack_lookup(std::unordered_map<uint64_t, std::vector<uint64_t>>& cache) const;
	KernelLookup kernel_lookup(std::map<std::pair<uint32_t, uint64_t>, std::string>& cache) const;
	std::optional<std::string> task_comm(uint32_t tgid, uint32_t tid) const;
	void gpu_blocked(uint64_t now, int64_t offset, const StackLookup& lookup, std::vector<model::OffCPUEvent>& out,
	                 std::set<uint32_t>& tids);
	void report_drops();
	void close_locked();
	policy::Signals policy_signals();
	policy::ApplyResult apply_policy(const policy::Decision& decision);
	uint64_t bpf_drop_count() const;
	uint32_t process_count() const;
	static int on_event(void* ctx, void* data, size_t size);

	Options opts_;
	std::string proc_root_;
	log::Logger log_;
	// gpu_ is false when GPU waits are disabled or the kernel lacks uprobe
	// attach cookies; the GPU programs are then not loaded.
	bool gpu_ = false;

	std::mutex mu_;
	std::unique_ptr<BpfObject> obj_;
	std::vector<std::unique_ptr<Link>> links_;
	uint32_t gen_ = 0;

	ring_buffer* rb_ = nullptr;
	std::mutex ev_mu_;
	std::vector<RawEvent> pending_;
	std::vector<RawEvent> batch_;  // owned by the reader thread
	std::jthread reader_;

	std::unique_ptr<uprobes::Manager> probes_;
	std::jthread probe_thread_;
	std::unique_ptr<uprobes::Manager> gpu_probes_;
	// launch_probes_ is reset once the launch rate exceeds the limit;
	// gpu_mu_ guards it and the rate state.
	std::mutex gpu_mu_;
	std::unique_ptr<uprobes::Manager> launch_probes_;
	uint64_t last_launch_hits_ = 0;
	std::chrono::steady_clock::time_point last_launch_check_{};
	std::jthread gpu_thread_;
	std::unique_ptr<unwinder::BpfStore> unwind_store_;
	std::unique_ptr<unwinder::Manager> unwind_;
	std::jthread unwind_thread_;

	prometheus::Family<prometheus::Gauge>& drops_;
	prometheus::Counter& entries_;
	prometheus::Counter& events_;
	prometheus::Counter& events_dropped_;
	prometheus::Family<prometheus::Gauge>& unwinder_mode_;
	prometheus::Gauge& gpu_libraries_;
	prometheus::Gauge& gpu_unsupported_;
	prometheus::Gauge& gpu_launch_tracking_;
	unwinder::Metrics unwind_metrics_;

	std::atomic<uint32_t> max_events_{20000};
	std::atomic<uint32_t> sample_percent_{100};
	std::atomic<uint32_t> unwind_depth_{64};
	std::atomic<int> active_unwind_mode_{0};
	bool dwarf_loaded_ = false;
	std::atomic<uint64_t> policy_accepted_events_{0};
	std::atomic<uint64_t> policy_capacity_drops_{0};
	std::atomic<uint64_t> policy_decode_drops_{0};
	std::unique_ptr<policy::Controller> policy_;
	std::jthread policy_thread_;
	prometheus::Gauge& event_limit_;
	prometheus::Gauge& event_sample_;
	prometheus::Gauge& unwind_depth_gauge_;
};

}  // namespace etrace::agent::offcpu
