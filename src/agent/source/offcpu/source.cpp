#include "agent/source/offcpu/source.h"

#include <bpf/libbpf.h>
#include <time.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <sstream>
#include <thread>

#include "agent/source/offcpu/gpu.h"
#include "platform/sync.h"

namespace etrace::agent::offcpu::objects {
extern const std::string_view offcpu_fp;
extern const std::string_view offcpu_dwarf;
}  // namespace etrace::agent::offcpu::objects

namespace etrace::agent::offcpu {

namespace {

constexpr const char* kDropReasons[] = {"stack", "counts", "start", "wake", "event", "chain", "gpu"};

// kMinUnwindRefresh bounds how often one process's mappings are re-read
// when the walker keeps meeting unknown addresses, e.g. in JIT code.
constexpr auto kMinUnwindRefresh = std::chrono::seconds(10);

uint64_t mono_now() {
	timespec ts{};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

int64_t mono_to_wall() {
	int64_t before = unix_nano(now());
	auto mono = static_cast<int64_t>(mono_now());
	int64_t after = unix_nano(now());
	return (before + after) / 2 - mono;
}

uint64_t ns(Duration d) {
	return static_cast<uint64_t>(d.count());
}

}  // namespace

Source::Source(Options o, Deps deps)
    : opts_(std::move(o)),
      proc_root_(deps.proc_root),
      log_(deps.log.with({{"source", "offcpu"}})),
      drops_(deps.registry.gauge_family("etrace_offcpu_bpf_drops")),
      entries_(deps.registry.counter("etrace_offcpu_aggregated_entries_total")),
      events_(deps.registry.counter("etrace_offcpu_events_total")),
      events_dropped_(deps.registry.counter("etrace_offcpu_events_dropped_total")),
      unwinder_mode_(deps.registry.gauge_family("etrace_offcpu_user_unwinder",
                                                "Set to 1 for the user stack unwinder in use.")),
      gpu_libraries_(deps.registry.gauge("etrace_offcpu_gpu_libraries",
                                         "CUDA and NCCL libraries with GPU wait probes attached.")),
      gpu_unsupported_(deps.registry.gauge("etrace_offcpu_gpu_unsupported",
                                           "Set to 1 when GPU waits are enabled but the kernel cannot run them.")),
      gpu_launch_tracking_(deps.registry.gauge("etrace_offcpu_gpu_launch_tracking",
                                               "Set to 1 while kernel launches are probed to name GPU waits.")),
      unwind_metrics_(deps.registry) {}

Source::~Source() {
	close();
}

void Source::configure(BpfObject& obj) {
	obj.set_const<uint32_t>("target_tgid", opts_.target_pid);
	obj.set_const<uint64_t>("min_block_ns", ns(opts_.min_block));
	obj.set_const<uint64_t>("max_block_ns", opts_.max_block > Duration(0) ? ns(opts_.max_block)
	                                                                      : std::numeric_limits<uint64_t>::max());
	obj.set_const<uint8_t>("include_preempted", opts_.include_preempted ? 1 : 0);
	obj.set_const<uint8_t>("capture_user_stacks", opts_.user_stacks ? 1 : 0);
	obj.set_const<uint8_t>("emit_traced_events", opts_.trace_context.enabled ? 1 : 0);
	obj.set_const<uint64_t>("slow_event_ns", ns(opts_.slow_event));
	obj.set_const<uint32_t>("chain_depth", static_cast<uint32_t>(opts_.chain_depth));
	obj.set_const<uint64_t>("gpu_min_wait_ns", ns(opts_.gpu.min_wait));
	for (const auto* p : {gpu::kEnterProg, gpu::kExitProg, gpu::kNcclProg}) {
		obj.set_autoload(p, gpu_);
	}
	for (const auto* p :
	     {gpu::kLaunchProg, gpu::kLaunchRetProg, gpu::kEventRecordProg, gpu::kFnEnterProg, gpu::kFnExitProg}) {
		obj.set_autoload(p, gpu_ && opts_.gpu.launches);
	}
}

std::unique_ptr<BpfObject> Source::load_object(bool dwarf) {
	auto obj = std::make_unique<BpfObject>(dwarf ? objects::offcpu_dwarf : objects::offcpu_fp,
	                                       dwarf ? "offcpu_dwarf" : "offcpu");
	configure(*obj);
	if (dwarf) {
		obj->set_max_entries("unwind_rows", static_cast<uint32_t>(opts_.unwind.max_rows));
		obj->set_max_entries("unwind_procs", static_cast<uint32_t>(opts_.unwind.max_processes));
	}
	obj->load();
	return obj;
}

// load picks the BPF object for the configured unwinder. In auto mode a
// kernel that rejects the DWARF walker (older than 5.17, or a verifier
// limit) gets the frame pointer object; the choice is logged and exported
// as etrace_offcpu_user_unwinder.
std::pair<std::unique_ptr<BpfObject>, std::string> Source::load() {
	if (!opts_.user_stacks || opts_.unwind.mode == kUnwindFramePointer) {
		return {load_object(false), kUnwindFramePointer};
	}
	try {
		return {load_object(true), kUnwindDwarf};
	} catch (const Error& e) {
		if (opts_.unwind.mode == kUnwindDwarf) {
			throw;
		}
		log_.warn("dwarf user stack unwinder unavailable, falling back to frame pointers", {{"err", e}});
	}
	return {load_object(false), kUnwindFramePointer};
}

void Source::start() {
	std::lock_guard lock(mu_);
	raise_memlock();
	gpu_ = opts_.gpu.enabled && attach_cookies_supported();
	if (opts_.gpu.enabled && !gpu_) {
		log_.warn("GPU waits disabled: the kernel does not pass attach cookies to uprobes (needs Linux 5.15)");
		gpu_unsupported_.Set(1);
	}
	auto [obj, mode] = load();
	obj_ = std::move(obj);
	unwinder_mode_.Add({{"mode", mode}}).Set(1);
	try {
		std::vector<std::string> progs{"on_switch", "on_waking", "on_exit"};
		if (mode == kUnwindDwarf) {
			progs.emplace_back("on_exec");
		}
		for (const auto& p : progs) {
			links_.push_back(obj_->attach(p));
		}
		rb_ = ring_buffer__new(obj_->map_fd("events"), &Source::on_event, this, nullptr);
		if (!rb_) {
			throw Error(std::format("open ring buffer: {}", std::strerror(errno)));
		}
		reader_ = std::jthread([this](std::stop_token st) { read_events(st); });
		if (mode == kUnwindDwarf) {
			start_unwinder();
		}
		if (opts_.trace_context.enabled) {
			probes_ = std::make_unique<uprobes::Manager>(
			        uprobes::Scanner{proc_root_, opts_.trace_context.library_patterns,
			                         opts_.trace_context.paths},
			        uprobes::single(opts_.trace_context.symbol, "on_tls_register"), "trace context", *obj_,
			        log_);
			probe_thread_ = std::jthread(
			        [this](std::stop_token st) { probes_->run(st, opts_.trace_context.scan_interval); });
		}
		if (gpu_) {
			start_gpu();
		}
	} catch (...) {
		close_locked();
		throw;
	}
}

void Source::start_gpu() {
	uprobes::Scanner sc{proc_root_, opts_.gpu.library_patterns, opts_.gpu.paths};
	gpu_probes_ = std::make_unique<uprobes::Manager>(sc, gpu::probes, "gpu", *obj_, log_);
	if (opts_.gpu.launches) {
		launch_probes_ =
		        std::make_unique<uprobes::Manager>(sc, gpu::launch_probes, "gpu launches", *obj_, log_);
		gpu_launch_tracking_.Set(1);
	}
	last_launch_check_ = std::chrono::steady_clock::now();
	gpu_thread_ = std::jthread([this](std::stop_token st) {
		do {
			sync_gpu();
		} while (sleep_for(st, opts_.gpu.scan_interval));
	});
}

// sync_gpu attaches GPU probes to new libraries. Launch probes are detached
// for good when launches since the last check exceeded max_launch_rate: each
// hit costs a uprobe trap on the thread that launches.
void Source::sync_gpu() {
	gpu_probes_->sync();
	gpu_libraries_.Set(static_cast<double>(gpu_probes_->attached()));
	std::lock_guard lock(gpu_mu_);
	if (!launch_probes_) {
		return;
	}
	auto now = std::chrono::steady_clock::now();
	auto hits = launch_hits();
	double secs = std::chrono::duration<double>(now - last_launch_check_).count();
	double rate = secs > 0 ? static_cast<double>(hits - last_launch_hits_) / secs : 0;
	last_launch_hits_ = hits;
	last_launch_check_ = now;
	if (rate > opts_.gpu.max_launch_rate) {
		log_.warn(
		        "GPU launch tracking stopped: launch rate above gpu.max_launch_rate; waits keep no kernel "
		        "names",
		        {{"rate", rate}, {"max", opts_.gpu.max_launch_rate}});
		launch_probes_->close();
		launch_probes_.reset();
		gpu_launch_tracking_.Set(0);
		return;
	}
	launch_probes_->sync();
}

uint64_t Source::launch_hits() {
	std::vector<uint64_t> per_cpu(static_cast<size_t>(std::max(libbpf_num_possible_cpus(), 1)));
	uint32_t zero = 0;
	if (bpf_map_lookup_elem(obj_->map_fd("launch_hits"), &zero, per_cpu.data()) != 0) {
		return last_launch_hits_;
	}
	uint64_t sum = 0;
	for (auto v : per_cpu) {
		sum += v;
	}
	return sum;
}

void Source::start_unwinder() {
	unwind_store_ = std::make_unique<unwinder::BpfStore>(obj_->map_fd("unwind_rows"), obj_->map_fd("unwind_procs"),
	                                                     obj_->map_fd("unwind_miss"));
	unwinder::Config cfg;
	cfg.proc_root = proc_root_;
	cfg.capacity = static_cast<uint32_t>(opts_.unwind.max_rows);
	cfg.min_refresh = kMinUnwindRefresh;
	cfg.loads_per_sync = 64;
	cfg.max_tables = 4096;
	unwind_ = std::make_unique<unwinder::Manager>(cfg, *unwind_store_, log_, unwind_metrics_);
	unwind_thread_ = std::jthread([this](std::stop_token st) {
		if (opts_.target_pid != 0) {
			unwind_->load(opts_.target_pid);
		}
		do {
			try {
				unwind_->sync();
			} catch (const std::exception& e) {
				log_.warn("unwind table sync failed", {{"err", e}});
			}
		} while (sleep_for(st, opts_.unwind.refresh_interval));
	});
}

int Source::on_event(void* ctx, void* data, size_t size) {
	auto* s = static_cast<Source*>(ctx);
	auto ev = decode_event(std::span(static_cast<const char*>(data), size));
	if (!ev) {
		s->events_dropped_.Increment();
		return 0;
	}
	s->batch_.push_back(*ev);
	return 0;
}

// read_events drains the ring buffer in batches: records are copied into a
// thread-owned buffer during a poll and moved to the pending list under one
// lock acquisition per poll.
void Source::read_events(std::stop_token st) {
	batch_.reserve(1024);
	while (!st.stop_requested()) {
		int n = ring_buffer__poll(rb_, 100);
		if (n < 0 && n != -EINTR) {
			log_.warn("ring buffer read failed", {{"err", std::strerror(-n)}});
			return;
		}
		if (batch_.empty()) {
			continue;
		}
		size_t accepted = 0, dropped = 0;
		{
			std::lock_guard lock(ev_mu_);
			auto room = static_cast<size_t>(opts_.max_events_per_interval) -
			            std::min(pending_.size(), static_cast<size_t>(opts_.max_events_per_interval));
			accepted = std::min(room, batch_.size());
			pending_.insert(pending_.end(), batch_.begin(),
			                batch_.begin() + static_cast<std::ptrdiff_t>(accepted));
			dropped = batch_.size() - accepted;
		}
		batch_.clear();
		if (accepted) {
			events_.Increment(static_cast<double>(accepted));
		}
		if (dropped) {
			events_dropped_.Increment(static_cast<double>(dropped));
		}
	}
}

StackLookup Source::stack_lookup(std::unordered_map<uint64_t, std::vector<uint64_t>>& cache) const {
	int fd = obj_->map_fd("stacks");
	return [fd, &cache](uint64_t id) -> const std::vector<uint64_t>& {
		auto [it, inserted] = cache.try_emplace(id);
		if (inserted) {
			RawStack st{};
			if (bpf_map_lookup_elem(fd, &id, &st) == 0) {
				it->second = stack_ips(st);
			}
		}
		return it->second;
	};
}

// kernel_lookup reads driver function names that on_gpu_fn_exit recorded.
KernelLookup Source::kernel_lookup(std::map<std::pair<uint32_t, uint64_t>, std::string>& cache) const {
	int fd = obj_->map_fd("kernel_names");
	return [fd, &cache](uint32_t tgid, uint64_t handle) -> std::string {
		auto [it, inserted] = cache.try_emplace({tgid, handle});
		if (inserted) {
			RawHandleKey key{tgid, 0, handle};
			RawKernelName n{};
			if (bpf_map_lookup_elem(fd, &key, &n) == 0) {
				it->second = cstr(n.name, sizeof n.name);
			}
		}
		return it->second;
	};
}

void Source::collect(model::Batch& b) {
	std::lock_guard lock(mu_);
	if (!obj_) {
		throw Error("source not started");
	}
	uint32_t idle = gen_ & 1;
	uint32_t generation = ++gen_;
	uint32_t zero = 0;
	if (bpf_map_update_elem(obj_->map_fd("generation"), &zero, &generation, BPF_ANY) != 0) {
		throw Error("set collection generation");
	}
	std::this_thread::sleep_for(opts_.drain_grace);

	std::unordered_map<uint64_t, std::vector<uint64_t>> stacks;
	auto lookup = stack_lookup(stacks);
	std::map<std::pair<uint32_t, uint64_t>, std::string> names;
	KernelLookup kernels;
	if (gpu_ && opts_.gpu.launches) {
		kernels = kernel_lookup(names);
	}
	int chains_fd = obj_->map_fd("chains");
	std::unordered_map<uint64_t, std::optional<RawChain>> chains;
	auto chain_of = [&](uint64_t id) -> const RawChain* {
		if (id == 0) {
			return nullptr;
		}
		auto [it, inserted] = chains.try_emplace(id);
		if (inserted) {
			RawChain c{};
			if (bpf_map_lookup_elem(chains_fd, &id, &c) == 0) {
				it->second = c;
			}
		}
		return it->second ? &*it->second : nullptr;
	};

	int counts = obj_->map_fd(std::format("counts_{}", idle));
	std::vector<RawKey> keys;
	RawKey key{}, next{};
	RawVal val{};
	const void* prev = nullptr;
	while (bpf_map_get_next_key(counts, prev, &next) == 0) {
		key = next;
		prev = &key;
		if (bpf_map_lookup_elem(counts, &key, &val) != 0) {
			continue;
		}
		b.offcpu.push_back(to_sample(key, val, chain_of(key.chain_id), lookup, kernels));
		keys.push_back(key);
	}
	if (errno != ENOENT) {
		throw Error(std::format("iterate counts: {}", std::strerror(errno)));
	}
	for (const auto& k : keys) {
		bpf_map_delete_elem(counts, &k);
	}
	entries_.Increment(static_cast<double>(keys.size()));

	std::vector<RawEvent> pending;
	{
		std::lock_guard ev_lock(ev_mu_);
		pending.swap(pending_);
	}
	auto offset = mono_to_wall();
	b.events.reserve(b.events.size() + pending.size());
	for (const auto& e : pending) {
		b.events.push_back(to_event(e, offset, lookup, kernels));
	}
	report_drops();
}

std::vector<model::OffCPUEvent> Source::blocked() {
	std::lock_guard lock(mu_);
	if (!obj_) {
		throw Error("source not started");
	}
	auto now = mono_now();
	auto offset = mono_to_wall();
	std::unordered_map<uint64_t, std::vector<uint64_t>> stacks;
	auto lookup = stack_lookup(stacks);
	std::vector<model::OffCPUEvent> out;
	std::set<uint32_t> in_gpu;
	if (gpu_) {
		gpu_blocked(now, offset, lookup, out, in_gpu);
	}
	int starts = obj_->map_fd("starts");
	uint32_t tid = 0, next = 0;
	const void* prev = nullptr;
	while (bpf_map_get_next_key(starts, prev, &next) == 0) {
		tid = next;
		prev = &tid;
		RawStart st{};
		if (bpf_map_lookup_elem(starts, &tid, &st) != 0) {
			continue;
		}
		if (st.ts > now || now - st.ts < ns(opts_.min_block) || (st.in_gpu && in_gpu.contains(tid))) {
			continue;
		}
		auto comm = task_comm(st.tgid, tid);
		if (!comm) {
			continue;
		}
		out.push_back(to_blocked(st, tid, *comm, now, offset, lookup));
	}
	if (errno != ENOENT) {
		throw Error(std::format("iterate blocked threads: {}", std::strerror(errno)));
	}
	return out;
}

std::optional<std::string> Source::task_comm(uint32_t tgid, uint32_t tid) const {
	std::ifstream in(std::format("{}/{}/task/{}/comm", proc_root_, tgid, tid));
	if (!in) {
		return std::nullopt;
	}
	std::string comm;
	std::getline(in, comm);
	while (!comm.empty() && std::isspace(static_cast<unsigned char>(comm.back()))) {
		comm.pop_back();
	}
	return comm;
}

// gpu_blocked lists the open GPU waits, named by the kernel queued last on
// what each waits for, and records their threads in tids.
void Source::gpu_blocked(uint64_t now, int64_t offset, const StackLookup& lookup, std::vector<model::OffCPUEvent>& out,
                         std::set<uint32_t>& tids) {
	std::map<std::pair<uint32_t, uint64_t>, std::string> names;
	KernelLookup kernels;
	if (opts_.gpu.launches) {
		kernels = kernel_lookup(names);
	}
	int waits = obj_->map_fd("gpu_waits");
	int by_stream = obj_->map_fd("stream_launches"), by_proc = obj_->map_fd("proc_launches"),
	    events = obj_->map_fd("event_streams");
	uint32_t tid = 0, next = 0;
	const void* prev = nullptr;
	while (bpf_map_get_next_key(waits, prev, &next) == 0) {
		tid = next;
		prev = &tid;
		RawGpuWait w{};
		if (bpf_map_lookup_elem(waits, &tid, &w) != 0 || w.depth == 0) {
			continue;
		}
		tids.insert(tid);
		if (w.ts > now || now - w.ts < ns(opts_.gpu.min_wait)) {
			continue;
		}
		auto comm = task_comm(w.tgid, tid);
		if (!comm) {
			continue;
		}
		uint64_t stream = 0;
		RawHandleKey hk{w.tgid, 0, w.obj};
		if (w.argkind == kGpuArgStream) {
			stream = w.obj;
		} else if (w.argkind == kGpuArgEvent && bpf_map_lookup_elem(events, &hk, &stream) != 0) {
			stream = 0;
		}
		RawLaunch l{};
		bool found;
		if (w.argkind != 0 && stream > 2) {
			hk.handle = stream;
			found = bpf_map_lookup_elem(by_stream, &hk, &l) == 0;
		} else {
			found = bpf_map_lookup_elem(by_proc, &w.tgid, &l) == 0;
		}
		found = found && l.ts <= w.ts;
		out.push_back(
		        to_gpu_blocked(w, tid, *comm, now, offset, lookup, kernels, found ? &l : nullptr, stream));
	}
	if (errno != ENOENT) {
		throw Error(std::format("iterate GPU waits: {}", std::strerror(errno)));
	}
}

size_t Source::probes_attached() {
	return probes_ ? probes_->attached() : 0;
}

void Source::sync_probes() {
	if (probes_) {
		probes_->sync();
	}
}

size_t Source::gpu_libraries_probed() {
	return gpu_probes_ ? gpu_probes_->attached() : 0;
}

size_t Source::gpu_launch_libraries_probed() {
	std::lock_guard lock(gpu_mu_);
	return launch_probes_ ? launch_probes_->attached() : 0;
}

void Source::sync_gpu_probes() {
	if (gpu_probes_) {
		sync_gpu();
	}
}

bool Source::unwind_loaded(uint32_t tgid) {
	std::lock_guard lock(mu_);
	if (!obj_) {
		return false;
	}
	unwinder::Proc p;
	return bpf_map_lookup_elem(obj_->map_fd("unwind_procs"), &tgid, &p) == 0;
}

void Source::report_drops() {
	int fd = obj_->map_fd("drops");
	for (uint32_t i = 0; i < std::size(kDropReasons); i++) {
		uint64_t v = 0;
		if (bpf_map_lookup_elem(fd, &i, &v) == 0) {
			drops_.Add({{"reason", kDropReasons[i]}}).Set(static_cast<double>(v));
		}
	}
}

void Source::close() {
	std::lock_guard lock(mu_);
	close_locked();
}

void Source::close_locked() {
	if (unwind_thread_.joinable()) {
		unwind_thread_.request_stop();
		unwind_thread_.join();
	}
	if (probe_thread_.joinable()) {
		probe_thread_.request_stop();
		probe_thread_.join();
	}
	if (probes_) {
		probes_->close();
		probes_.reset();
	}
	if (gpu_thread_.joinable()) {
		gpu_thread_.request_stop();
		gpu_thread_.join();
	}
	if (gpu_probes_) {
		gpu_probes_->close();
		gpu_probes_.reset();
	}
	{
		std::lock_guard gl(gpu_mu_);
		if (launch_probes_) {
			launch_probes_->close();
			launch_probes_.reset();
		}
	}
	if (reader_.joinable()) {
		reader_.request_stop();
		reader_.join();
	}
	if (rb_) {
		ring_buffer__free(rb_);
		rb_ = nullptr;
	}
	links_.clear();
	unwind_.reset();
	unwind_store_.reset();
	obj_.reset();
}

}  // namespace etrace::agent::offcpu
