#include <gtest/gtest.h>

#include <format>
#include <sstream>

#include "agent/source/offcpu/unwinder.h"
#include "symbol/elf.h"
#include "symbol/resolver.h"
#include "symbol/table.h"
#include "testutil.h"
#include "unwind/unwind.h"

namespace etrace {
namespace {

namespace fs = std::filesystem;

const std::vector<uint8_t> kTestID = {0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89};
const std::string kTestIDHex = "abcdef0123456789";
const std::vector<testing::TestSym> kWorkSym = {{"work", 0x1000, 0x100}};

TEST(Symbol, Kallsyms) {
	std::istringstream in(
	        "ffffffff81000000 T _stext\nffffffff81001000 t schedule\nffffffff81002000 D some_data\n"
	        "ffffffff81003000 T tcp_sendmsg\t[tcp]\n");
	auto t = symbol::parse_kallsyms(in);
	EXPECT_EQ(t->size(), 3u);
	EXPECT_EQ(*t->lookup(0xffffffff81001010), "schedule");
	EXPECT_EQ(*t->lookup(0xffffffff81003001), "tcp_sendmsg [tcp]");
	EXPECT_EQ(t->lookup(0x10), nullptr) << "address below first symbol must not resolve";
}

TEST(Symbol, ParseMaps) {
	std::istringstream in(
	        "55d0c0000000-55d0c0021000 r-xp 00002000 08:01 1234 /usr/bin/my app\n"
	        "7ffd00000000-7ffd00021000 rw-p 00000000 00:00 0 [stack]\n");
	auto maps = symbol::parse_maps(in);
	const auto* m = symbol::find_mapping(maps, 0x55d0c0000010);
	ASSERT_NE(m, nullptr);
	EXPECT_EQ(m->path, "/usr/bin/my app");
	EXPECT_TRUE(m->exec);
	EXPECT_EQ(m->offset, 0x2000u);
}

TEST(Symbol, BuildIDOfStrippedObject) {
	testing::TempDir dir;
	testing::write_elf(dir / "bin", kTestID, {});
	auto f = symbol::ElfFile::open(dir / "bin");
	EXPECT_EQ(symbol::build_id(*f), kTestIDHex);
	auto ef = symbol::load_elf_info(dir / "bin");
	EXPECT_FALSE(ef->complete);
	EXPECT_EQ(ef->build_id, kTestIDHex);
}

// ProcFixture lays out /proc/1 with a stripped /app/bin mapped at 0x400000.
struct ProcFixture {
	testing::TempDir proc;
	std::string root = proc / "1/root";
	ProcFixture() {
		testing::write_elf(root + "/app/bin", kTestID, {});
		testing::write_file(proc / "1/maps", "400000-410000 r-xp 00000000 00:00 1 /app/bin\n");
	}
	symbol::Config config() const {
		symbol::Config c;
		c.proc_root = proc.path();
		c.process_cache = 4;
		c.elf_cache = 4;
		return c;
	}
};

TEST(Resolver, UsesDistributionDebugFile) {
	ProcFixture f;
	testing::write_elf(
	        f.root + "/usr/lib/debug/.build-id/" + kTestIDHex.substr(0, 2) + "/" + kTestIDHex.substr(2) + ".debug",
	        kTestID, kWorkSym);
	symbol::ProcResolver r(f.config());
	auto got = r.user(1, 0x401010);
	EXPECT_EQ(got.function, "work");
	EXPECT_EQ(got.module, "bin");
}

class FakeFinder : public symbol::DebugFinder {
public:
	std::string path;
	int calls = 0;
	std::optional<std::string> find(const std::string&) override {
		calls++;
		return path.empty() ? std::nullopt : std::optional(path);
	}
};

TEST(Resolver, RetriesDebugFinder) {
	ProcFixture f;
	FakeFinder finder;
	auto now = std::chrono::steady_clock::time_point{} + 1h;
	auto cfg = f.config();
	cfg.debug = &finder;
	cfg.clock = [&] { return now; };
	symbol::ProcResolver r(cfg);
	EXPECT_EQ(r.user(1, 0x401010).function, "") << "stripped object resolved";
	testing::TempDir dbg;
	testing::write_elf(dbg / "x.debug", kTestID, kWorkSym);
	finder.path = dbg / "x.debug";
	r.user(1, 0x401010);
	EXPECT_EQ(finder.calls, 1) << "finder consulted before the retry delay";
	now += symbol::kDebugRetry;
	EXPECT_EQ(r.user(1, 0x401010).function, "work");
}

TEST(Resolver, RejectsMismatchedDebugFile) {
	ProcFixture f;
	testing::write_elf(
	        f.root + "/usr/lib/debug/.build-id/" + kTestIDHex.substr(0, 2) + "/" + kTestIDHex.substr(2) + ".debug",
	        {1, 2, 3, 4}, kWorkSym);
	symbol::ProcResolver r(f.config());
	EXPECT_EQ(r.user(1, 0x401010).function, "");
}

TEST(Resolver, KeepsSymbolsAfterProcessExit) {
	ProcFixture f;
	testing::write_elf(f.root + "/app/bin", kTestID, kWorkSym);
	auto cfg = f.config();
	cfg.process_ttl = 1min;
	symbol::ProcResolver r(cfg);
	EXPECT_EQ(r.user(1, 0x401010).function, "work");
	fs::remove_all(f.proc / "1");
	EXPECT_EQ(r.user(1, 0x401020).function, "work") << "after exit";
}

// EhBuilder assembles .eh_frame entries.
struct EhBuilder {
	std::string buf;
	static void put32(std::string& b, uint32_t v) { b.append(reinterpret_cast<const char*>(&v), 4); }

	size_t entry(const std::function<uint32_t(size_t)>& id, const std::string& payload) {
		size_t start = buf.size();
		put32(buf, static_cast<uint32_t>(4 + payload.size()));
		put32(buf, id(buf.size()));
		buf += payload;
		return start;
	}
	size_t cie(const std::string& payload) {
		return entry([](size_t) { return 0u; }, payload);
	}
	void fde(size_t cie, const std::string& payload) {
		entry([cie](size_t body) { return static_cast<uint32_t>(body - cie); }, payload);
	}
	std::string fde_body(uint64_t sec, uint64_t begin, uint32_t size, const std::string& prog) {
		size_t field = buf.size() + 8;
		std::string out;
		put32(out, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int64_t>(begin) -
		                                                      static_cast<int64_t>(sec + field))));
		put32(out, size);
		out += '\0';
		return out + prog;
	}
};

// x86-64 CIE as emitted by GCC: "zR", code align 1, data align -8, return
// address column 16, pc-relative sdata4 FDE pointers, CFA = rsp+8 and the
// return address at CFA-8.
const std::string kX86CIE("\x01zR\x00\x01\x78\x10\x01\x1b\x0c\x07\x08\x90\x01", 14);

std::string bytes(std::initializer_list<int> v) {
	std::string out;
	for (int b : v) {
		out += static_cast<char>(b);
	}
	return out;
}

TEST(Unwind, ParseX86Prologue) {
	const uint64_t sec = 0x2000;
	EhBuilder b;
	auto c = b.cie(kX86CIE);
	auto prog = bytes({0x41, 0x0e, 16, 0x86, 2, 0x43, 0x0d, 6, 0x0a, 0x50, 0x0c, 7, 8, 0x41, 0x0b});
	b.fde(c, b.fde_body(sec, 0x1000, 0x20, prog));
	auto rows = unwind::normalize(unwind::parse_eh_frame(b.buf, sec, unwind::Arch::x86_64));
	using unwind::CFAKind;
	unwind::Rule ra{unwind::RuleKind::offset, -8}, fp{unwind::RuleKind::offset, -16};
	std::vector<unwind::Row> want{
	        {0x1000, CFAKind::sp, 8, {}, ra}, {0x1001, CFAKind::sp, 16, fp, ra}, {0x1004, CFAKind::fp, 16, fp, ra},
	        {0x1014, CFAKind::sp, 8, fp, ra}, {0x1015, CFAKind::fp, 16, fp, ra}, {0x1020, CFAKind::none, 0, {}, {}},
	};
	ASSERT_EQ(rows.size(), want.size());
	for (size_t i = 0; i < want.size(); i++) {
		EXPECT_EQ(rows[i].pc, want[i].pc) << i;
		EXPECT_TRUE(rows[i].same_rules(want[i])) << i;
	}
	EXPECT_FALSE(unwind::lookup(rows, 0x1020)) << "address past the function must not resolve";
	EXPECT_FALSE(unwind::lookup(rows, 0xfff)) << "address before the function must not resolve";
	auto r = unwind::lookup(rows, 0x1010);
	ASSERT_TRUE(r);
	EXPECT_EQ(r->cfa, CFAKind::fp);
}

TEST(Unwind, AdjacentFunctionsAndUnsupportedRules) {
	const uint64_t sec = 0x4000;
	EhBuilder b;
	auto c = b.cie(kX86CIE);
	b.fde(c, b.fde_body(sec, 0x1000, 0x10, ""));
	b.fde(c, b.fde_body(sec, 0x1010, 0x10, bytes({0x0f, 2, 0x77, 8})));
	b.fde(c, b.fde_body(sec, 0x1030, 0x10, bytes({0x41, 0x10, 6, 1, 0x00})));
	b.buf += std::string(4, '\0');
	auto rows = unwind::normalize(unwind::parse_eh_frame(b.buf, sec, unwind::Arch::x86_64));
	EXPECT_EQ(unwind::lookup(rows, 0x100f)->cfa, unwind::CFAKind::sp);
	EXPECT_EQ(unwind::lookup(rows, 0x1010)->cfa, unwind::CFAKind::unsupported);
	EXPECT_FALSE(unwind::lookup(rows, 0x1025)) << "gap between functions must not resolve";
	EXPECT_EQ(unwind::lookup(rows, 0x1031)->fp.kind, unwind::RuleKind::undefined);
}

TEST(Unwind, MalformedInput) {
	for (const auto& data : {bytes({0xff, 0xff, 0xff, 0xff}), bytes({0x10, 0, 0, 0, 0, 0, 0, 0}), bytes({4})}) {
		EXPECT_THROW(unwind::parse_eh_frame(data, 0, unwind::Arch::x86_64), Error);
	}
	EhBuilder b;
	auto c = b.cie(kX86CIE);
	b.fde(c, b.fde_body(0, 0x1000, 0x10, bytes({0x0b})));
	b.fde(c, b.fde_body(0, 0x2000, 0x10, bytes({0x3f, 0x05})));
	EXPECT_TRUE(unwind::parse_eh_frame(b.buf, 0, unwind::Arch::x86_64).empty()) << "invalid FDEs must be skipped";
	EXPECT_THROW(unwind::arch_of(3), unwind::UnsupportedArch);
}

TEST(Unwind, BuildTableARM64Binary) {
	auto path = testing::scenario_dir() + "/lockchain-arm64";
	if (!fs::exists(path)) {
		GTEST_SKIP() << "scenario binary not built";
	}
	auto f = symbol::ElfFile::open(path);
	auto rows = unwind::build_table(*f);
	ASSERT_GE(rows.size(), 100u);
	for (size_t i = 1; i < rows.size(); i++) {
		ASSERT_GT(rows[i].pc, rows[i - 1].pc) << "rows not strictly sorted at " << i;
	}
	int checked = 0;
	for (const auto& s : f->symbols(symbol::kShtSymtab)) {
		if (s.name != "holder" && s.name != "waiter" && s.name != "sleeper") {
			continue;
		}
		auto entry = unwind::lookup(rows, s.value);
		ASSERT_TRUE(entry) << s.name;
		EXPECT_EQ(entry->cfa, unwind::CFAKind::sp);
		EXPECT_EQ(entry->cfa_offset, 0);
		EXPECT_EQ(entry->ra.kind, unwind::RuleKind::same);
		bool saved = false;
		for (auto pc = s.value; pc < s.value + s.size && !saved; pc += 4) {
			auto r = unwind::lookup(rows, pc);
			saved = r && r->ra.kind == unwind::RuleKind::offset && r->fp.kind == unwind::RuleKind::offset;
		}
		EXPECT_TRUE(saved) << s.name << " never saves x29/x30";
		checked++;
	}
	EXPECT_EQ(checked, 3);
}

namespace uw = agent::offcpu::unwinder;

class FakeStore : public uw::Store {
public:
	std::map<uint32_t, uw::Row> rows;
	int writes = 0;
	std::map<uint32_t, uw::Proc> procs;
	std::vector<uint32_t> misses;
	bool put_fail = false;

	void write_rows(uint32_t off, const std::vector<uw::Row>& rs) override {
		writes++;
		for (size_t i = 0; i < rs.size(); i++) {
			rows[off + static_cast<uint32_t>(i)] = rs[i];
		}
	}
	void put_proc(uint32_t tgid, const uw::Proc& p) override {
		if (put_fail) {
			throw Error("map full");
		}
		procs[tgid] = p;
	}
	void delete_proc(uint32_t tgid) override { procs.erase(tgid); }
	std::vector<uint32_t> drain_misses() override { return std::exchange(misses, {}); }
};

constexpr uint64_t kLibBase = 0x7f0000000000;

uw::Segment exec_segment(const std::string& path) {
	auto f = symbol::ElfFile::open(path);
	auto segs = uw::exec_segments(*f);
	if (segs.empty()) {
		throw Error(path + " has no executable segment");
	}
	return segs[0];
}

uint32_t table_rows(const std::string& path) {
	auto f = symbol::ElfFile::open(path);
	return static_cast<uint32_t>(unwind::build_table(*f).size());
}

// map_line renders the /proc/<pid>/maps line the loader creates for the
// executable segment of a file loaded at base.
std::string map_line(const uw::Segment& seg, uint64_t base, const std::string& path) {
	constexpr uint64_t page = 0xfff;
	auto start = base + (seg.vaddr & ~page);
	auto end = base + ((seg.vaddr + seg.filesz + page) & ~page);
	return std::format("{:x}-{:x} r-xp {:08x} 00:00 1 {}\n", start, end, seg.off & ~page, path);
}

struct UnwindFixture {
	testing::TempDir proc;
	std::string scen = testing::scenario_dir();
	uw::Segment exe, lib;
	telemetry::Registry reg{false};
	uw::Metrics metrics{reg};

	static bool available() {
		auto d = testing::scenario_dir();
		return fs::exists(d + "/lockchain-arm64") && fs::exists(d + "/libetrace_ctx.so");
	}
	UnwindFixture() : exe(exec_segment(scen + "/lockchain-arm64")), lib(exec_segment(scen + "/libetrace_ctx.so")) {}

	void add_proc(uint32_t tgid, const std::vector<std::string>& lines) {
		auto dir = proc / std::to_string(tgid);
		fs::create_directories(dir);
		fs::create_directory_symlink(scen, dir + "/root");
		std::string maps;
		for (const auto& l : lines) {
			maps += l;
		}
		testing::write_file(dir + "/maps", maps);
	}
	std::string exe_line() const { return map_line(exe, 0, "/lockchain-arm64"); }
	std::string lib_line() const { return map_line(lib, kLibBase, "/libetrace_ctx.so"); }
	std::unique_ptr<uw::Manager> manager(uw::Store& s, uint32_t capacity,
	                                     uw::Manager::Clock clock = std::chrono::steady_clock::now) {
		uw::Config c;
		c.proc_root = proc.path();
		c.capacity = capacity;
		c.min_refresh = 10s;
		c.loads_per_sync = 8;
		c.max_tables = 16;
		return std::make_unique<uw::Manager>(c, s, log::Logger::discard(), metrics, std::move(clock));
	}
};

#define REQUIRE_SCENARIOS()                                    \
	if (!UnwindFixture::available()) {                     \
		GTEST_SKIP() << "scenario binaries not built"; \
	}

TEST(Unwinder, LoadBuildsSortedMappingsAndSharesTables) {
	REQUIRE_SCENARIOS();
	UnwindFixture f;
	f.add_proc(100, {f.exe_line(), "ffff0000-ffff1000 r-xp 00000000 00:00 0 [vdso]\n", f.lib_line()});
	f.add_proc(101, {f.lib_line()});
	FakeStore s;
	auto m = f.manager(s, 1 << 20);
	s.misses = {100, 101};
	m->sync();
	const auto& p = s.procs[100];
	ASSERT_EQ(p.n, 3u);
	const auto &exe = p.mappings[0], &vdso = p.mappings[1], &lib = p.mappings[2];
	EXPECT_EQ(exe.row_cnt, table_rows(f.scen + "/lockchain-arm64"));
	EXPECT_EQ(exe.bias, 0u);
	EXPECT_EQ(vdso.row_cnt, 0u) << "vdso must have no table";
	EXPECT_NE(lib.row_cnt, 0u);
	EXPECT_EQ(lib.bias, kLibBase);
	EXPECT_TRUE(exe.start < vdso.start && vdso.start < lib.start);
	const auto& shared = s.procs[101].mappings[0];
	EXPECT_EQ(shared.row_off, lib.row_off) << "library table not shared";
	EXPECT_EQ(shared.row_cnt, lib.row_cnt);
	EXPECT_EQ(s.writes, 2) << "rows written once per object";
	EXPECT_EQ(s.rows.size(), exe.row_cnt + lib.row_cnt);
}

TEST(Unwinder, RefreshIsRateLimited) {
	REQUIRE_SCENARIOS();
	UnwindFixture f;
	f.add_proc(100, {f.lib_line()});
	FakeStore s;
	auto now = std::chrono::steady_clock::time_point{} + 1000s;
	auto m = f.manager(s, 1 << 20, [&] { return now; });
	s.misses = {100};
	m->sync();
	s.procs.erase(100);
	s.misses = {100};
	now += 1s;
	m->sync();
	EXPECT_FALSE(s.procs.contains(100)) << "reload within min_refresh";
	s.misses = {100};
	now += 10s;
	m->sync();
	EXPECT_TRUE(s.procs.contains(100)) << "reload after min_refresh missing";
}

TEST(Unwinder, ExitedProcessReleasesTablesForReuse) {
	REQUIRE_SCENARIOS();
	UnwindFixture f;
	auto exe_rows = table_rows(f.scen + "/lockchain-arm64");
	f.add_proc(100, {f.exe_line()});
	FakeStore s;
	auto m = f.manager(s, exe_rows);
	s.misses = {100};
	m->sync();
	ASSERT_EQ(s.procs[100].mappings[0].row_cnt, exe_rows);
	fs::remove_all(f.proc / "100");
	m->sync();
	EXPECT_FALSE(s.procs.contains(100)) << "exited process still stored";
	f.add_proc(200, {f.lib_line()});
	s.misses = {200};
	m->sync();
	EXPECT_NE(s.procs[200].mappings[0].row_cnt, 0u) << "idle table was not evicted to make room";
	EXPECT_EQ(m->table_count(), 1u);
}

TEST(Unwinder, TableInUseIsNotEvicted) {
	REQUIRE_SCENARIOS();
	UnwindFixture f;
	auto exe_rows = table_rows(f.scen + "/lockchain-arm64");
	f.add_proc(100, {f.exe_line()});
	f.add_proc(200, {f.lib_line()});
	FakeStore s;
	auto m = f.manager(s, exe_rows);
	s.misses = {100, 200};
	m->sync();
	EXPECT_EQ(s.procs[100].mappings[0].row_cnt, exe_rows);
	EXPECT_EQ(s.procs[200].mappings[0].row_cnt, 0u)
	        << "lib must fall back to frame pointers while the table is full";
}

TEST(Unwinder, FailedStoreKeepsNoReferences) {
	REQUIRE_SCENARIOS();
	UnwindFixture f;
	f.add_proc(100, {f.lib_line()});
	FakeStore s;
	s.put_fail = true;
	auto m = f.manager(s, 1 << 20);
	s.misses = {100, 300};
	m->sync();
	EXPECT_EQ(m->referenced_tables(), 0u);
	EXPECT_FALSE(m->tracks(300)) << "missing process must not be tracked";
}

TEST(Unwinder, EncodeRow) {
	auto r = uw::encode({unwind::Row{
	        1, unwind::CFAKind::fp, 16, {unwind::RuleKind::offset, -16}, {unwind::RuleKind::offset, -8}}})[0];
	EXPECT_EQ(r.cfa, uw::kCfaFP);
	EXPECT_EQ(r.cfa_offset, 16);
	EXPECT_EQ(r.fp_offset, -16);
	EXPECT_EQ(r.ra_offset, -8);
	EXPECT_EQ(r.rules, 0x11);
	EXPECT_EQ(uw::encode({unwind::Row{1, unwind::CFAKind::sp, 1 << 20, {}, {}}})[0].cfa, uw::kCfaUnsupported);
	EXPECT_EQ(uw::encode({unwind::Row{1, unwind::CFAKind::sp, 0, {}, {unwind::RuleKind::undefined, 0}}})[0].rules,
	          0x20);
}

TEST(Unwinder, Allocator) {
	uw::Allocator a(10);
	auto x = a.alloc(4), y = a.alloc(4);
	ASSERT_TRUE(x && y);
	EXPECT_FALSE(a.alloc(4)) << "over-allocation";
	a.release(*x, 4);
	EXPECT_FALSE(a.alloc(3)) << "quarantined range reused before flush";
	a.flush();
	a.free(*y, 4);
	auto all = a.alloc(10);
	ASSERT_TRUE(all) << "free ranges not merged";
	EXPECT_EQ(*all, 0u);
	EXPECT_EQ(a.used(), 10u);
}

}  // namespace
}  // namespace etrace
