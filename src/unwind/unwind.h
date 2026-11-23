#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "platform/error.h"
#include "symbol/elf.h"

namespace etrace::unwind {

// The unwind module turns the call frame information of an ELF object into
// a flat, address-sorted table that a stack walker can binary search.

class UnsupportedArch : public Error {
public:
	using Error::Error;
};

class NoUnwindInfo : public Error {
public:
	NoUnwindInfo() : Error("unwind: no .eh_frame section") {}
};

enum class Arch : uint8_t { x86_64 = 1, arm64 = 2 };

Arch arch_of(uint16_t machine);

enum class CFAKind : uint8_t {
	// none marks an address range without unwind information.
	none = 0,
	sp = 1,
	fp = 2,
	// unsupported covers DWARF expressions and CFA bases other than the
	// stack or frame pointer, which the walker cannot evaluate.
	unsupported = 3,
};

enum class RuleKind : uint8_t {
	// same means the register still holds the caller's value. For the
	// arm64 return address this is the link register of a leaf frame.
	same = 0,
	// offset means the caller's value is saved at CFA+offset.
	offset = 1,
	// undefined means the value cannot be recovered; for the return
	// address it marks the outermost frame.
	undefined = 2,
};

struct Rule {
	RuleKind kind = RuleKind::same;
	int32_t offset = 0;
	bool operator==(const Rule&) const = default;
};

// Row describes how to unwind every address from pc up to the pc of the
// next row in the table. pc is an ELF virtual address.
struct Row {
	uint64_t pc = 0;
	CFAKind cfa = CFAKind::none;
	int32_t cfa_offset = 0;
	Rule fp;
	Rule ra;

	bool same_rules(const Row& o) const {
		return cfa == o.cfa && cfa_offset == o.cfa_offset && fp == o.fp && ra == o.ra;
	}
};

// parse_eh_frame walks every CIE and FDE in data (mapped at addr) and
// returns the unsorted rows of all FDEs, each followed by a CFANone row at
// the function end. An FDE that cannot be interpreted is skipped rather
// than failing the object so the remaining functions stay unwindable.
std::vector<Row> parse_eh_frame(std::string_view data, uint64_t addr, Arch arch);

// normalize sorts rows, lets a function's first row win over the end marker
// of the preceding function at the same address, and merges neighbours with
// identical rules.
std::vector<Row> normalize(std::vector<Row> rows);

// build_table parses .eh_frame of f. Stripped binaries keep this section
// because the C++ runtime needs it, so it is the most widely available
// source of unwind information.
std::vector<Row> build_table(symbol::ElfFile& f);

std::optional<Row> lookup(const std::vector<Row>& rows, uint64_t pc);

}  // namespace etrace::unwind
