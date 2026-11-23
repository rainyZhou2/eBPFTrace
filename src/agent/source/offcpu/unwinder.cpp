#include "agent/source/offcpu/unwinder.h"

#include <bpf/bpf.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>

namespace fs = std::filesystem;

namespace etrace::agent::offcpu::unwinder {

namespace {

std::optional<int16_t> fit16(int32_t v) {
	if (v < std::numeric_limits<int16_t>::min() || v > std::numeric_limits<int16_t>::max()) {
		return std::nullopt;
	}
	return static_cast<int16_t>(v);
}

bool encode_rule(const unwind::Rule& r, uint8_t& kind, int16_t& off) {
	kind = static_cast<uint8_t>(r.kind);
	off = 0;
	if (r.kind != unwind::RuleKind::offset) {
		return true;
	}
	auto v = fit16(r.offset);
	if (!v) {
		return false;
	}
	off = *v;
	return true;
}

Row encode_row(const unwind::Row& r) {
	Row out;
	out.pc = r.pc;
	switch (r.cfa) {
		case unwind::CFAKind::none:
			out.cfa = kCfaNone;
			return out;
		case unwind::CFAKind::sp:
			out.cfa = kCfaSP;
			break;
		case unwind::CFAKind::fp:
			out.cfa = kCfaFP;
			break;
		default:
			out.cfa = kCfaUnsupported;
			return out;
	}
	auto cfa = fit16(r.cfa_offset);
	uint8_t fp_rule, ra_rule;
	int16_t fp_off, ra_off;
	bool ok2 = encode_rule(r.fp, fp_rule, fp_off);
	bool ok3 = encode_rule(r.ra, ra_rule, ra_off);
	if (!cfa || !ok2 || !ok3) {
		Row u;
		u.pc = r.pc;
		u.cfa = kCfaUnsupported;
		return u;
	}
	out.cfa_offset = *cfa;
	out.fp_offset = fp_off;
	out.ra_offset = ra_off;
	out.rules = static_cast<uint8_t>(fp_rule | ra_rule << 4);
	return out;
}

std::string errstr() {
	return std::strerror(errno);
}

}  // namespace

std::vector<Row> encode(const std::vector<unwind::Row>& rows) {
	std::vector<Row> out;
	out.reserve(rows.size());
	for (const auto& r : rows) {
		out.push_back(encode_row(r));
	}
	return out;
}

std::vector<Segment> exec_segments(const symbol::ElfFile& f) {
	std::vector<Segment> out;
	for (const auto& p : f.progs()) {
		if (p.type == symbol::kPtLoad && (p.flags & symbol::kPfX)) {
			out.push_back(Segment{p.offset, p.vaddr, p.filesz});
		}
	}
	return out;
}

std::optional<uint64_t> bias(const std::vector<Segment>& segs, uint64_t start, uint64_t end, uint64_t offset) {
	for (const auto& s : segs) {
		if (offset < s.off + s.filesz && s.off < offset + (end - start)) {
			return start - offset + s.off - s.vaddr;
		}
	}
	return std::nullopt;
}

Allocator::Allocator(uint32_t capacity) : capacity_(capacity) {
	if (capacity > 0) {
		free_.push_back(Extent{0, capacity});
	}
}

std::optional<uint32_t> Allocator::alloc(uint32_t n) {
	if (n == 0) {
		return std::nullopt;
	}
	for (size_t i = 0; i < free_.size(); i++) {
		auto e = free_[i];
		if (e.n < n) {
			continue;
		}
		if (e.n == n) {
			free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(i));
		} else {
			free_[i] = Extent{e.off + n, e.n - n};
		}
		used_ += n;
		return e.off;
	}
	return std::nullopt;
}

void Allocator::release(uint32_t off, uint32_t n) {
	if (n > 0) {
		quarantine_.push_back(Extent{off, n});
	}
}

void Allocator::free(uint32_t off, uint32_t n) {
	if (n == 0) {
		return;
	}
	insert(Extent{off, n});
	used_ -= n;
}

void Allocator::flush() {
	for (const auto& e : quarantine_) {
		insert(e);
		used_ -= e.n;
	}
	quarantine_.clear();
}

void Allocator::insert(Extent e) {
	auto it = std::upper_bound(free_.begin(), free_.end(), e.off,
	                           [](uint32_t off, const Extent& x) { return off < x.off; });
	size_t i = static_cast<size_t>(it - free_.begin());
	free_.insert(it, e);
	if (i + 1 < free_.size() && free_[i].off + free_[i].n == free_[i + 1].off) {
		free_[i].n += free_[i + 1].n;
		free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(i + 1));
	}
	if (i > 0 && free_[i - 1].off + free_[i - 1].n == free_[i].off) {
		free_[i - 1].n += free_[i].n;
		free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(i));
	}
}

// writeChunk keeps a single batch syscall bounded in kernel memory use.
constexpr size_t kWriteChunk = 16384;

void BpfStore::write_rows(uint32_t off, const std::vector<Row>& rows) {
	std::vector<uint32_t> keys;
	for (size_t start = 0; start < rows.size(); start += kWriteChunk) {
		size_t end = std::min(start + kWriteChunk, rows.size());
		keys.resize(end - start);
		for (size_t i = 0; i < keys.size(); i++) {
			keys[i] = off + static_cast<uint32_t>(start + i);
		}
		auto count = static_cast<__u32>(keys.size());
		if (bpf_map_update_batch(rows_, keys.data(), rows.data() + start, &count, nullptr) != 0) {
			throw Error(std::format("update rows {}..{}: {}", keys.front(), keys.back(), errstr()));
		}
	}
}

void BpfStore::put_proc(uint32_t tgid, const Proc& p) {
	if (bpf_map_update_elem(procs_, &tgid, &p, BPF_ANY) != 0) {
		throw Error(std::format("update proc {}: {}", tgid, errstr()));
	}
}

void BpfStore::delete_proc(uint32_t tgid) {
	if (bpf_map_delete_elem(procs_, &tgid) != 0 && errno != ENOENT) {
		throw Error(std::format("delete proc {}: {}", tgid, errstr()));
	}
}

std::vector<uint32_t> BpfStore::drain_misses() {
	std::vector<uint32_t> out;
	uint32_t key, next;
	const void* prev = nullptr;
	while (bpf_map_get_next_key(misses_, prev, &next) == 0) {
		out.push_back(next);
		key = next;
		prev = &key;
	}
	for (auto k : out) {
		if (bpf_map_delete_elem(misses_, &k) != 0 && errno != ENOENT) {
			throw Error(std::format("delete miss {}: {}", k, errstr()));
		}
	}
	return out;
}

Metrics::Metrics(telemetry::Registry& reg)
    : loads(reg.counter_family("etrace_unwind_process_loads_total", "Process mapping loads by result.")),
      table_full(reg.counter("etrace_unwind_table_full_total",
                             "Objects left to frame-pointer unwinding because the row table was full.")),
      truncated(reg.counter("etrace_unwind_mappings_truncated_total",
                            "Process loads that exceeded the per-process mapping limit.")),
      rows_used(reg.gauge("etrace_unwind_rows_used")),
      tables(reg.gauge("etrace_unwind_tables")),
      procs(reg.gauge("etrace_unwind_processes")) {}

struct Manager::Table {
	std::string key;
	uint32_t off = 0, n = 0;
	std::vector<Segment> segs;
	int refs = 0;
	std::optional<std::list<Table*>::iterator> idle;
	uint64_t idle_cycle = 0;
};

Manager::Manager(Config cfg, Store& store, log::Logger log, Metrics& m, Clock clock)
    : cfg_(std::move(cfg)),
      store_(store),
      log_(std::move(log)),
      m_(m),
      now_(std::move(clock)),
      alloc_(cfg_.capacity),
      keys_(static_cast<size_t>(cfg_.max_tables) * 4, Duration(0)) {}

Manager::~Manager() = default;

size_t Manager::referenced_tables() const {
	size_t n = 0;
	for (const auto& [_, t] : tables_) {
		n += t->refs > 0 ? 1 : 0;
	}
	return n;
}

std::string Manager::proc_path(uint32_t tgid, const std::string& rest) const {
	auto p = fs::path(cfg_.proc_root) / std::to_string(tgid);
	if (!rest.empty()) {
		p /= fs::path(rest).relative_path();
	}
	return p.string();
}

void Manager::sync() {
	cycle_++;
	alloc_.flush();
	std::vector<uint32_t> misses;
	try {
		misses = store_.drain_misses();
	} catch (const Error& e) {
		throw Error(std::format("drain unwind misses: {}", e.what()));
	}
	auto now = now_();
	int loads = 0;
	for (auto tgid : misses) {
		if (auto it = procs_.find(tgid); it != procs_.end() && now - it->second.loaded < cfg_.min_refresh) {
			continue;
		}
		if (loads >= cfg_.loads_per_sync) {
			break;
		}
		loads++;
		load(tgid, now);
	}
	collect_exited();
	m_.rows_used.Set(alloc_.used());
	m_.tables.Set(static_cast<double>(tables_.size()));
	m_.procs.Set(static_cast<double>(procs_.size()));
}

void Manager::load(uint32_t tgid) {
	load(tgid, now_());
}

void Manager::load(uint32_t tgid, std::chrono::steady_clock::time_point now) {
	std::ifstream in(proc_path(tgid, "maps"));
	if (!in) {
		m_.loads.Add({{"result", "gone"}}).Increment();
		drop(tgid);
		return;
	}
	auto maps = symbol::parse_maps(in);
	Proc p;
	std::vector<Table*> refs;
	for (const auto& mp : maps) {
		if (!mp.exec) {
			continue;
		}
		if (p.n == kMaxMappings) {
			m_.truncated.Increment();
			break;
		}
		Mapping e{mp.start, mp.end, 0, 0, 0};
		if (auto* t = acquire(tgid, mp)) {
			refs.push_back(t);
			if (t->n > 0) {
				if (auto b = bias(t->segs, mp.start, mp.end, mp.offset)) {
					e.bias = *b;
					e.row_off = t->off;
					e.row_cnt = t->n;
				}
			}
		}
		p.mappings[p.n++] = e;
	}
	std::vector<Table*> old;
	if (auto it = procs_.find(tgid); it != procs_.end()) {
		old = std::move(it->second.tables);
	}
	try {
		store_.put_proc(tgid, p);
		m_.loads.Add({{"result", "ok"}}).Increment();
	} catch (const Error& e) {
		m_.loads.Add({{"result", "store_failed"}}).Increment();
		log_.debug("store unwind mappings failed", {{"tgid", tgid}, {"err", e}});
		release_all(refs);
		refs.clear();
	}
	procs_[tgid] = ProcState{std::move(refs), now};
	release_all(old);
}

void Manager::drop(uint32_t tgid) {
	auto it = procs_.find(tgid);
	if (it == procs_.end()) {
		return;
	}
	auto tables = std::move(it->second.tables);
	procs_.erase(it);
	try {
		store_.delete_proc(tgid);
	} catch (const Error& e) {
		log_.debug("delete unwind mappings failed", {{"tgid", tgid}, {"err", e}});
	}
	release_all(tables);
}

void Manager::collect_exited() {
	std::vector<uint32_t> gone;
	for (const auto& [tgid, _] : procs_) {
		std::error_code ec;
		if (!fs::exists(proc_path(tgid), ec) && !ec) {
			gone.push_back(tgid);
		}
	}
	for (auto tgid : gone) {
		drop(tgid);
	}
}

// acquire returns the referenced table of the file behind a mapping, or
// nullptr when the mapping has no regular file or its table did not fit.
Manager::Table* Manager::acquire(uint32_t tgid, const symbol::Mapping& mp) {
	if (mp.path.empty() || mp.path[0] != '/' || mp.path.ends_with(" (deleted)")) {
		return nullptr;
	}
	auto full = proc_path(tgid, "root" + mp.path);
	struct stat st{};
	if (::stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
		return nullptr;
	}
	auto file_key =
	        std::format("{}:{}:{}", full, st.st_size, int64_t{st.st_mtim.tv_sec} * 1000000000 + st.st_mtim.tv_nsec);
	if (auto key = keys_.get(file_key)) {
		if (auto it = tables_.find(*key); it != tables_.end()) {
			ref(it->second.get());
			return it->second.get();
		}
	}
	std::unique_ptr<symbol::ElfFile> f;
	try {
		f = symbol::ElfFile::open(full);
	} catch (const Error&) {
		return nullptr;
	}
	auto key = symbol::build_id(*f);
	if (key.empty()) {
		key = "file:" + file_key;
	}
	keys_.add(file_key, key);
	if (auto it = tables_.find(key); it != tables_.end()) {
		ref(it->second.get());
		return it->second.get();
	}
	auto t = std::make_unique<Table>();
	t->key = key;
	t->segs = exec_segments(*f);
	std::vector<unwind::Row> rows;
	try {
		rows = unwind::build_table(*f);
	} catch (const Error& e) {
		log_.debug("no unwind table", {{"path", mp.path}, {"err", e}});
	}
	if (auto enc = encode(rows); !enc.empty()) {
		auto n = static_cast<uint32_t>(enc.size());
		auto off = allocate(n);
		if (!off) {
			m_.table_full.Increment();
			return nullptr;
		}
		try {
			store_.write_rows(*off, enc);
		} catch (const Error& e) {
			alloc_.free(*off, n);
			log_.warn("write unwind rows failed", {{"path", mp.path}, {"err", e}});
			return nullptr;
		}
		t->off = *off;
		t->n = n;
	}
	auto* raw = t.get();
	tables_[key] = std::move(t);
	ref(raw);
	trim_idle();
	return raw;
}

// allocate evicts tables that have been idle since an earlier cycle until n
// rows fit. Their rows are unreachable from any process entry stored before
// this cycle, so they can be reused at once.
std::optional<uint32_t> Manager::allocate(uint32_t n) {
	if (n > alloc_.capacity()) {
		return std::nullopt;
	}
	while (true) {
		if (auto off = alloc_.alloc(n)) {
			return off;
		}
		Table* victim = nullptr;
		for (auto it = idle_.rbegin(); it != idle_.rend(); ++it) {
			if ((*it)->n > 0 && (*it)->idle_cycle < cycle_) {
				victim = *it;
				break;
			}
		}
		if (!victim) {
			return std::nullopt;
		}
		evict(victim, true);
	}
}

void Manager::trim_idle() {
	while (tables_.size() > static_cast<size_t>(cfg_.max_tables) && !idle_.empty()) {
		evict(idle_.back(), false);
	}
}

void Manager::evict(Table* t, bool immediate) {
	if (t->idle) {
		idle_.erase(*t->idle);
		t->idle.reset();
	}
	auto off = t->off, n = t->n;
	tables_.erase(t->key);
	if (immediate) {
		alloc_.free(off, n);
	} else {
		alloc_.release(off, n);
	}
}

void Manager::ref(Table* t) {
	if (t->idle) {
		idle_.erase(*t->idle);
		t->idle.reset();
	}
	t->refs++;
}

void Manager::release_all(const std::vector<Table*>& ts) {
	for (auto* t : ts) {
		t->refs--;
		auto it = tables_.find(t->key);
		if (t->refs == 0 && it != tables_.end() && it->second.get() == t) {
			idle_.push_front(t);
			t->idle = idle_.begin();
			t->idle_cycle = cycle_;
		}
	}
}

}  // namespace etrace::agent::offcpu::unwinder
