#include "unwind/unwind.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <limits>
#include <map>
#include <string>

namespace etrace::unwind {

namespace {

constexpr uint8_t kPePtrAbs = 0x00;
constexpr uint8_t kPeULEB128 = 0x01;
constexpr uint8_t kPeUData2 = 0x02;
constexpr uint8_t kPeUData4 = 0x03;
constexpr uint8_t kPeUData8 = 0x04;
constexpr uint8_t kPeSLEB128 = 0x09;
constexpr uint8_t kPeSData2 = 0x0a;
constexpr uint8_t kPeSData4 = 0x0b;
constexpr uint8_t kPeSData8 = 0x0c;
constexpr uint8_t kPePCRel = 0x10;
constexpr uint8_t kPeOmit = 0xff;
constexpr uint8_t kPeFormatMask = 0x0f;
constexpr uint8_t kPeApplyMask = 0x70;

// DWARF register numbers of the registers the walker tracks.
struct Regs {
	uint64_t sp, fp, ra;
};

Regs regs_of(Arch a) {
	if (a == Arch::arm64) {
		return {31, 29, 30};
	}
	return {7, 6, 16};
}

// Reader decodes DWARF primitives. Any out-of-bounds read sets failed and
// moves to the end, so loops terminate and callers check failed once.
struct Reader {
	std::string_view buf;
	size_t pos = 0;
	uint64_t base = 0;  // virtual address of buf[0], for pc-relative pointers
	bool failed = false;

	void fail() {
		failed = true;
		pos = buf.size();
	}

	std::string_view bytes(uint64_t n) {
		if (n > buf.size() - pos) {
			fail();
			return {};
		}
		auto b = buf.substr(pos, static_cast<size_t>(n));
		pos += static_cast<size_t>(n);
		return b;
	}

	template <class T>
	T fixed() {
		auto b = bytes(sizeof(T));
		if (b.size() != sizeof(T)) {
			return 0;
		}
		T v;
		std::memcpy(&v, b.data(), sizeof v);
		return v;
	}

	uint8_t u8() { return fixed<uint8_t>(); }
	uint16_t u16() { return fixed<uint16_t>(); }
	uint32_t u32() { return fixed<uint32_t>(); }
	uint64_t u64() { return fixed<uint64_t>(); }

	uint64_t uleb() {
		uint64_t v = 0;
		for (unsigned shift = 0;; shift += 7) {
			uint8_t b = u8();
			if (failed) {
				return 0;
			}
			if (shift < 64) {
				v |= uint64_t{b & 0x7fu} << shift;
			}
			if (!(b & 0x80)) {
				return v;
			}
		}
	}

	int64_t sleb() {
		uint64_t v = 0;
		unsigned shift = 0;
		uint8_t b;
		while (true) {
			b = u8();
			if (failed) {
				return 0;
			}
			if (shift < 64) {
				v |= uint64_t{b & 0x7fu} << shift;
			}
			shift += 7;
			if (!(b & 0x80)) {
				break;
			}
		}
		if (shift < 64 && (b & 0x40)) {
			v |= ~uint64_t{0} << shift;
		}
		return static_cast<int64_t>(v);
	}

	std::string cstring() {
		auto end = buf.find('\0', pos);
		if (end == std::string_view::npos) {
			fail();
			return "";
		}
		std::string s(buf.substr(pos, end - pos));
		pos = end + 1;
		return s;
	}

	// pointer decodes a DW_EH_PE encoded value. Only absolute and
	// pc-relative application is supported; the others need section
	// addresses .eh_frame does not carry and do not occur in FDE addresses
	// emitted by GCC or LLVM.
	std::optional<uint64_t> pointer(uint8_t enc) {
		if (enc == kPeOmit) {
			return 0;
		}
		uint64_t field_addr = base + pos;
		uint64_t v;
		switch (enc & kPeFormatMask) {
			case kPePtrAbs:
			case kPeUData8:
			case kPeSData8:
				v = u64();
				break;
			case kPeULEB128:
				v = uleb();
				break;
			case kPeUData2:
				v = u16();
				break;
			case kPeUData4:
				v = u32();
				break;
			case kPeSLEB128:
				v = static_cast<uint64_t>(sleb());
				break;
			case kPeSData2:
				v = static_cast<uint64_t>(int64_t{static_cast<int16_t>(u16())});
				break;
			case kPeSData4:
				v = static_cast<uint64_t>(int64_t{static_cast<int32_t>(u32())});
				break;
			default:
				return std::nullopt;
		}
		switch (enc & kPeApplyMask) {
			case 0:
				break;
			case kPePCRel:
				v += field_addr;
				break;
			default:
				return std::nullopt;
		}
		if (failed) {
			return std::nullopt;
		}
		return v;
	}
};

struct Cie {
	uint64_t code_align = 0;
	int64_t data_align = 0;
	uint64_t ra_reg = 0;
	uint8_t fde_enc = kPePtrAbs;
	bool has_aug = false;
	std::string_view initial;
};

std::optional<Cie> parse_cie(Reader& r, size_t end) {
	Cie c;
	uint8_t version = r.u8();
	if (version != 1 && version != 3) {
		return std::nullopt;
	}
	auto aug = r.cstring();
	if (aug.find("eh") != std::string::npos) {
		return std::nullopt;
	}
	c.code_align = r.uleb();
	c.data_align = r.sleb();
	c.ra_reg = version == 1 ? r.u8() : r.uleb();
	if (!aug.empty()) {
		if (aug[0] != 'z') {
			return std::nullopt;
		}
		c.has_aug = true;
		uint64_t n = r.uleb();
		if (r.failed || n > end - std::min(end, r.pos)) {
			return std::nullopt;
		}
		size_t aug_end = r.pos + static_cast<size_t>(n);
		for (size_t i = 1; i < aug.size(); i++) {
			char ch = aug[i];
			if (ch == 'R') {
				c.fde_enc = r.u8();
			} else if (ch == 'P') {
				if (!r.pointer(r.u8() & ~uint8_t{0x80})) {
					return std::nullopt;
				}
			} else if (ch == 'L') {
				r.u8();
			} else if (ch == 'S' || ch == 'B' || ch == 'G') {
			} else {
				break;
			}
		}
		r.pos = aug_end;
	}
	if (r.failed || r.pos > end) {
		return std::nullopt;
	}
	c.initial = r.buf.substr(r.pos, end - r.pos);
	return c;
}

struct CfaState {
	uint64_t reg = 0;
	int64_t offset = 0;
	bool expr = false;
};

struct RegState {
	RuleKind kind = RuleKind::same;
	int64_t offset = 0;
};

struct State {
	CfaState cfa;
	RegState fp;
	RegState ra;
};

constexpr size_t kMaxRememberDepth = 64;
const RegState kUnknownReg{RuleKind::undefined, 0};

Rule to_rule(const RegState& rs) {
	if (rs.kind != RuleKind::offset) {
		return Rule{rs.kind, 0};
	}
	if (rs.offset < std::numeric_limits<int32_t>::min() || rs.offset > std::numeric_limits<int32_t>::max()) {
		return Rule{RuleKind::undefined, 0};
	}
	return Rule{RuleKind::offset, static_cast<int32_t>(rs.offset)};
}

// Machine interprets DWARF call frame instructions for the registers the
// walker needs: the CFA, the frame pointer and the return address.
struct Machine {
	const Cie& c;
	Regs regs;
	uint64_t loc = 0;
	State state;
	State initial;
	std::vector<State> stack;
	std::vector<Row> rows;

	void set_reg(uint64_t reg, RegState rs) {
		if (reg == regs.fp) {
			state.fp = rs;
		} else if (reg == c.ra_reg) {
			state.ra = rs;
		}
	}

	void restore(uint64_t reg) {
		if (reg == regs.fp) {
			state.fp = initial.fp;
		} else if (reg == c.ra_reg) {
			state.ra = initial.ra;
		}
	}

	void advance(uint64_t to) {
		if (to == loc) {
			return;
		}
		emit();
		loc = to;
	}

	void emit() { rows.push_back(row()); }

	Row row() const {
		Row r{loc, CFAKind::unsupported, 0, to_rule(state.fp), to_rule(state.ra)};
		if (state.cfa.expr || state.cfa.offset < std::numeric_limits<int32_t>::min() ||
		    state.cfa.offset > std::numeric_limits<int32_t>::max()) {
			r.cfa = CFAKind::unsupported;
		} else if (state.cfa.reg == regs.sp) {
			r.cfa = CFAKind::sp;
		} else if (state.cfa.reg == regs.fp) {
			r.cfa = CFAKind::fp;
		}
		if (r.cfa != CFAKind::unsupported) {
			r.cfa_offset = static_cast<int32_t>(state.cfa.offset);
		}
		return r;
	}

	// run executes one instruction stream, emitting a row whenever the
	// location advances. It returns false on malformed or unknown input.
	bool run(std::string_view prog, uint64_t begin) {
		Reader r{prog};
		auto da = c.data_align;
		while (r.pos < prog.size() && !r.failed) {
			uint8_t op = r.u8();
			uint64_t low = op & 0x3f;
			switch (op >> 6) {
				case 1:
					advance(loc + low * c.code_align);
					continue;
				case 2:
					set_reg(low, RegState{RuleKind::offset, static_cast<int64_t>(r.uleb()) * da});
					continue;
				case 3:
					restore(low);
					continue;
			}
			switch (op) {
				case 0x00:  // nop
					break;
				case 0x01: {  // set_loc
					// The operand is an absolute address in the CIE pointer
					// format; a pc-relative value would be relative to the
					// program bytes, which are not at a known address here.
					auto v = r.pointer(c.fde_enc & ~kPePCRel);
					if (!v || *v < begin) {
						return false;
					}
					advance(*v);
					break;
				}
				case 0x02:
					advance(loc + r.u8() * c.code_align);
					break;
				case 0x03:
					advance(loc + r.u16() * c.code_align);
					break;
				case 0x04:
					advance(loc + r.u32() * c.code_align);
					break;
				case 0x05: {  // offset_extended
					auto reg = r.uleb();
					set_reg(reg, RegState{RuleKind::offset, static_cast<int64_t>(r.uleb()) * da});
					break;
				}
				case 0x06:  // restore_extended
					restore(r.uleb());
					break;
				case 0x07:  // undefined
					set_reg(r.uleb(), RegState{RuleKind::undefined, 0});
					break;
				case 0x08:  // same_value
					set_reg(r.uleb(), RegState{RuleKind::same, 0});
					break;
				case 0x09: {  // register
					auto reg = r.uleb();
					r.uleb();
					set_reg(reg, kUnknownReg);
					break;
				}
				case 0x0a:  // remember_state
					if (stack.size() >= kMaxRememberDepth) {
						return false;
					}
					stack.push_back(state);
					break;
				case 0x0b:  // restore_state
					if (stack.empty()) {
						return false;
					}
					// The CFA rule is restored along with the registers,
					// matching libgcc and LLVM libunwind.
					state = stack.back();
					stack.pop_back();
					break;
				case 0x0c: {  // def_cfa
					auto reg = r.uleb();
					state.cfa = CfaState{reg, static_cast<int64_t>(r.uleb()), false};
					break;
				}
				case 0x0d:  // def_cfa_register
					state.cfa.reg = r.uleb();
					state.cfa.expr = false;
					break;
				case 0x0e:  // def_cfa_offset
					state.cfa.offset = static_cast<int64_t>(r.uleb());
					break;
				case 0x0f:  // def_cfa_expression
					r.bytes(r.uleb());
					state.cfa.expr = true;
					break;
				case 0x10: {  // expression
					auto reg = r.uleb();
					r.bytes(r.uleb());
					set_reg(reg, kUnknownReg);
					break;
				}
				case 0x11: {  // offset_extended_sf
					auto reg = r.uleb();
					set_reg(reg, RegState{RuleKind::offset, r.sleb() * da});
					break;
				}
				case 0x12: {  // def_cfa_sf
					auto reg = r.uleb();
					state.cfa = CfaState{reg, r.sleb() * da, false};
					break;
				}
				case 0x13:  // def_cfa_offset_sf
					state.cfa.offset = r.sleb() * da;
					break;
				case 0x14: {  // val_offset
					auto reg = r.uleb();
					r.uleb();
					set_reg(reg, kUnknownReg);
					break;
				}
				case 0x15: {  // val_offset_sf
					auto reg = r.uleb();
					r.sleb();
					set_reg(reg, kUnknownReg);
					break;
				}
				case 0x16: {  // val_expression
					auto reg = r.uleb();
					r.bytes(r.uleb());
					set_reg(reg, kUnknownReg);
					break;
				}
				case 0x2d:  // GNU_window_save, AArch64 negate_ra_state; PAC bits are stripped by the
				            // walker
					break;
				case 0x2e:  // GNU_args_size
					r.uleb();
					break;
				case 0x2f: {  // GNU_negative_offset_extended
					auto reg = r.uleb();
					set_reg(reg, RegState{RuleKind::offset, -static_cast<int64_t>(r.uleb()) * da});
					break;
				}
				default:
					return false;
			}
		}
		return !r.failed;
	}
};

std::vector<Row> parse_fde(Reader& r, size_t end, const Cie& c, Arch arch) {
	auto begin = r.pointer(c.fde_enc);
	if (!begin) {
		return {};
	}
	auto size = r.pointer(c.fde_enc & kPeFormatMask);
	if (!size || *begin == 0 || *size == 0) {
		return {};
	}
	if (c.has_aug) {
		uint64_t n = r.uleb();
		if (r.failed || n > end - std::min(end, r.pos)) {
			return {};
		}
		r.pos += static_cast<size_t>(n);
	}
	if (r.failed || r.pos > end) {
		return {};
	}
	Machine m{c, regs_of(arch)};
	if (!m.run(c.initial, *begin)) {
		return {};
	}
	m.initial = m.state;
	m.rows.clear();
	m.loc = *begin;
	if (!m.run(r.buf.substr(r.pos, end - r.pos), *begin)) {
		return {};
	}
	m.emit();
	m.rows.push_back(Row{*begin + *size, CFAKind::none, 0, {}, {}});
	return std::move(m.rows);
}

}  // namespace

Arch arch_of(uint16_t machine) {
	switch (machine) {
		case symbol::kMachineX86_64:
			return Arch::x86_64;
		case symbol::kMachineAArch64:
			return Arch::arm64;
	}
	throw UnsupportedArch(std::format("unwind: unsupported architecture: machine {}", machine));
}

std::vector<Row> parse_eh_frame(std::string_view data, uint64_t addr, Arch arch) {
	std::map<size_t, Cie> cies;
	std::vector<Row> rows;
	Reader r{data, 0, addr};
	while (r.pos < data.size()) {
		size_t start = r.pos;
		uint64_t length = r.u32();
		if (r.failed) {
			throw Error("unwind: malformed .eh_frame");
		}
		if (length == 0) {
			break;
		}
		if (length == 0xffffffffu) {
			throw Error("unwind: 64-bit .eh_frame entries are not supported");
		}
		size_t body = r.pos;
		if (length > data.size() - body) {
			throw Error("unwind: malformed .eh_frame");
		}
		size_t end = body + static_cast<size_t>(length);
		uint32_t id = r.u32();
		if (id == 0) {
			if (auto c = parse_cie(r, end)) {
				cies[start] = *c;
			}
		} else if (id <= body) {
			if (auto it = cies.find(body - id); it != cies.end()) {
				auto fde = parse_fde(r, end, it->second, arch);
				rows.insert(rows.end(), fde.begin(), fde.end());
			}
		}
		r.failed = false;
		r.pos = end;
	}
	return rows;
}

std::vector<Row> normalize(std::vector<Row> rows) {
	std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
		if (a.pc != b.pc) {
			return a.pc < b.pc;
		}
		return a.cfa == CFAKind::none && b.cfa != CFAKind::none;
	});
	std::vector<Row> out;
	out.reserve(rows.size());
	for (const auto& r : rows) {
		if (!out.empty() && out.back().pc == r.pc) {
			out.back() = r;
			continue;
		}
		out.push_back(r);
	}
	std::vector<Row> merged;
	merged.reserve(out.size());
	for (const auto& r : out) {
		if (!merged.empty() && merged.back().same_rules(r)) {
			continue;
		}
		if (merged.empty() && r.cfa == CFAKind::none) {
			continue;
		}
		merged.push_back(r);
	}
	return merged;
}

std::vector<Row> build_table(symbol::ElfFile& f) {
	auto arch = arch_of(f.machine());
	const auto* sec = f.section(".eh_frame");
	if (!sec || sec->type == symbol::kShtNobits) {
		throw NoUnwindInfo();
	}
	std::string_view data;
	try {
		data = f.data(*sec);
	} catch (const Error& e) {
		throw Error(std::format("read .eh_frame: {}", e.what()));
	}
	return normalize(parse_eh_frame(data, sec->addr, arch));
}

std::optional<Row> lookup(const std::vector<Row>& rows, uint64_t pc) {
	auto it = std::upper_bound(rows.begin(), rows.end(), pc, [](uint64_t p, const Row& r) { return p < r.pc; });
	if (it == rows.begin()) {
		return std::nullopt;
	}
	--it;
	if (it->cfa == CFAKind::none) {
		return std::nullopt;
	}
	return *it;
}

}  // namespace etrace::unwind
