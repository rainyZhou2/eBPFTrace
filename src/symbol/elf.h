#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "platform/error.h"

namespace etrace::symbol {

constexpr uint16_t kMachineX86_64 = 62;
constexpr uint16_t kMachineAArch64 = 183;

struct ElfSection {
	std::string name;
	uint32_t type = 0;
	uint64_t flags = 0;
	uint64_t addr = 0;
	uint64_t offset = 0;
	uint64_t size = 0;
	uint32_t link = 0;
	uint64_t entsize = 0;
};

struct ElfProg {
	uint32_t type = 0;
	uint32_t flags = 0;
	uint64_t offset = 0;
	uint64_t vaddr = 0;
	uint64_t filesz = 0;
};

struct ElfSymbol {
	std::string_view name;
	uint64_t value = 0;
	uint64_t size = 0;
	uint8_t info = 0;
};

constexpr uint32_t kShtNobits = 8;
constexpr uint32_t kShtNote = 7;
constexpr uint32_t kShtSymtab = 2;
constexpr uint32_t kShtDynsym = 11;
constexpr uint32_t kPtLoad = 1;
constexpr uint32_t kPtNote = 4;
constexpr uint32_t kPfX = 1;
constexpr uint8_t kSttFunc = 2;

// ElfFile is a read-only view of a 64-bit little-endian ELF object mapped
// into memory. Section data is bounds-checked against the file size so
// truncated or corrupt objects raise Error instead of reading past the end.
class ElfFile {
public:
	// open maps path and parses the headers; it throws Error for files
	// that are not ELF64 little-endian.
	static std::unique_ptr<ElfFile> open(const std::string& path);
	~ElfFile();
	ElfFile(const ElfFile&) = delete;
	ElfFile& operator=(const ElfFile&) = delete;

	uint16_t machine() const { return machine_; }
	const std::vector<ElfSection>& sections() const { return sections_; }
	const std::vector<ElfProg>& progs() const { return progs_; }
	const ElfSection* section(std::string_view name) const;

	// data returns a section's bytes, inflating SHF_COMPRESSED sections.
	std::string_view data(const ElfSection& s);
	std::string_view segment(const ElfProg& p) const;

	// symbols returns the .symtab or .dynsym entries; names point into the
	// mapping and stay valid while the file is open.
	std::vector<ElfSymbol> symbols(uint32_t type);

private:
	ElfFile() = default;
	std::string_view bytes(uint64_t off, uint64_t size) const;

	const char* base_ = nullptr;
	size_t size_ = 0;
	uint16_t machine_ = 0;
	std::vector<ElfSection> sections_;
	std::vector<ElfProg> progs_;
	std::vector<std::unique_ptr<std::string>> inflated_;
};

// build_id returns the hex GNU build ID, or "" when there is none. Note
// sections are tried first and PT_NOTE segments second, so both unstripped
// and section-stripped objects are covered.
std::string build_id(ElfFile& f);

}  // namespace etrace::symbol
