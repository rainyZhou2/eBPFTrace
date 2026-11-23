#pragma once

#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "platform/log.h"
#include "platform/lru.h"
#include "platform/telemetry.h"
#include "symbol/elf.h"
#include "symbol/table.h"
#include "unwind/unwind.h"

namespace etrace::agent::offcpu::unwinder {

// The unwinder keeps the BPF stack walker's view of process mappings and
// .eh_frame tables in sync with the processes it encounters.

// kMaxMappings mirrors MAX_UNWIND_MAPPINGS in offcpu.bpf.c.
inline constexpr uint32_t kMaxMappings = 64;

// Row mirrors struct unwind_row.
struct Row {
	uint64_t pc = 0;
	int16_t cfa_offset = 0;
	int16_t fp_offset = 0;
	int16_t ra_offset = 0;
	uint8_t cfa = 0;
	uint8_t rules = 0;
	bool operator==(const Row&) const = default;
};
static_assert(sizeof(Row) == 16);

// Mapping mirrors struct unwind_mapping.
struct Mapping {
	uint64_t start = 0;
	uint64_t end = 0;
	uint64_t bias = 0;
	uint32_t row_off = 0;
	uint32_t row_cnt = 0;
};
static_assert(sizeof(Mapping) == 32);

// Proc mirrors struct proc_unwind.
struct Proc {
	uint32_t n = 0;
	uint32_t pad = 0;
	Mapping mappings[kMaxMappings];
};

inline constexpr uint8_t kCfaNone = 0;
inline constexpr uint8_t kCfaSP = 1;
inline constexpr uint8_t kCfaFP = 2;
inline constexpr uint8_t kCfaUnsupported = 3;

// encode converts rows to the BPF layout. Offsets that do not fit 16 bits
// mark the row unsupported, which makes the walker use frame pointers there.
std::vector<Row> encode(const std::vector<unwind::Row>& rows);

// Segment is the part of a PT_LOAD header needed to translate addresses.
struct Segment {
	uint64_t off = 0;
	uint64_t vaddr = 0;
	uint64_t filesz = 0;
};

std::vector<Segment> exec_segments(const symbol::ElfFile& f);

// bias returns the value that turns a runtime address in the mapping into
// an ELF virtual address (vaddr = pc - bias). The executable segment is the
// one whose file range overlaps the mapping's; the mapping offset is page
// aligned while p_offset need not be, so containment cannot be required.
std::optional<uint64_t> bias(const std::vector<Segment>& segs, uint64_t start, uint64_t end, uint64_t offset);

// Allocator hands out row ranges of the shared BPF table. Freed ranges are
// quarantined until the next flush so a walker that looked up a mapping just
// before it was replaced never reads rows of a different object.
class Allocator {
public:
	explicit Allocator(uint32_t capacity);
	std::optional<uint32_t> alloc(uint32_t n);
	void release(uint32_t off, uint32_t n);
	// free returns a range no walker can still be reading straight to the
	// pool.
	void free(uint32_t off, uint32_t n);
	void flush();
	uint32_t used() const { return used_; }
	uint32_t capacity() const { return capacity_; }

private:
	struct Extent {
		uint32_t off, n;
	};
	void insert(Extent e);

	uint32_t capacity_;
	std::vector<Extent> free_;  // sorted by offset, non-adjacent
	std::vector<Extent> quarantine_;
	uint32_t used_ = 0;
};

// Store is the BPF side of the walker: the row table, per-process mapping
// entries and the set of processes the walker could not fully resolve.
class Store {
public:
	virtual ~Store() = default;
	virtual void write_rows(uint32_t off, const std::vector<Row>& rows) = 0;
	virtual void put_proc(uint32_t tgid, const Proc& p) = 0;
	virtual void delete_proc(uint32_t tgid) = 0;
	virtual std::vector<uint32_t> drain_misses() = 0;
};

class BpfStore : public Store {
public:
	BpfStore(int rows_fd, int procs_fd, int misses_fd) : rows_(rows_fd), procs_(procs_fd), misses_(misses_fd) {}
	void write_rows(uint32_t off, const std::vector<Row>& rows) override;
	void put_proc(uint32_t tgid, const Proc& p) override;
	void delete_proc(uint32_t tgid) override;
	std::vector<uint32_t> drain_misses() override;

private:
	int rows_, procs_, misses_;
};

struct Config {
	std::string proc_root;
	// capacity is the number of rows of the BPF table.
	uint32_t capacity = 0;
	// min_refresh rate-limits reloading one process, which the walker asks
	// for whenever it meets an address outside the known mappings.
	Duration min_refresh{0};
	// loads_per_sync bounds the work of one sync; deferred processes are
	// reported again by the walker.
	int loads_per_sync = 64;
	// max_tables bounds cached tables, including objects without unwind
	// information.
	int max_tables = 4096;
};

struct Metrics {
	explicit Metrics(telemetry::Registry& reg);
	prometheus::Family<prometheus::Counter>& loads;
	prometheus::Counter& table_full;
	prometheus::Counter& truncated;
	prometheus::Gauge& rows_used;
	prometheus::Gauge& tables;
	prometheus::Gauge& procs;
};

// Manager is driven by a single thread through sync.
class Manager {
public:
	using Clock = std::function<std::chrono::steady_clock::time_point()>;
	Manager(Config cfg, Store& store, log::Logger log, Metrics& m, Clock clock = std::chrono::steady_clock::now);
	~Manager();

	// sync loads the processes the walker reported and forgets exited ones.
	void sync();
	// load reads the mappings of tgid immediately, bypassing the rate limit.
	void load(uint32_t tgid);

	size_t table_count() const { return tables_.size(); }
	bool tracks(uint32_t tgid) const { return procs_.contains(tgid); }
	// referenced_tables counts tables some process still uses.
	size_t referenced_tables() const;

private:
	struct Table;
	struct ProcState {
		std::vector<Table*> tables;
		std::chrono::steady_clock::time_point loaded;
	};

	std::string proc_path(uint32_t tgid, const std::string& rest = "") const;
	void load(uint32_t tgid, std::chrono::steady_clock::time_point now);
	void drop(uint32_t tgid);
	void collect_exited();
	Table* acquire(uint32_t tgid, const symbol::Mapping& mp);
	std::optional<uint32_t> allocate(uint32_t n);
	void trim_idle();
	void evict(Table* t, bool immediate);
	void ref(Table* t);
	void release_all(const std::vector<Table*>& ts);

	Config cfg_;
	Store& store_;
	log::Logger log_;
	Metrics& m_;
	Clock now_;
	Allocator alloc_;
	uint64_t cycle_ = 0;
	std::unordered_map<std::string, std::unique_ptr<Table>> tables_;
	std::list<Table*> idle_;
	LruCache<std::string, std::string> keys_;
	std::map<uint32_t, ProcState> procs_;
};

}  // namespace etrace::agent::offcpu::unwinder
