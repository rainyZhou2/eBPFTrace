#include "symbol/elf.h"

#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <cerrno>
#include <cstring>
#include <format>

#include "platform/hex.h"

namespace etrace::symbol {

namespace {

template <class T>
T load(const char* p) {
	T v;
	std::memcpy(&v, p, sizeof v);
	return v;
}

}  // namespace

std::unique_ptr<ElfFile> ElfFile::open(const std::string& path) {
	int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		throw Error(std::format("open {}: {}", path, std::strerror(errno)));
	}
	struct stat st{};
	if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
		::close(fd);
		throw Error(std::format("{}: not a regular file", path));
	}
	if (st.st_size < static_cast<off_t>(sizeof(Elf64_Ehdr))) {
		::close(fd);
		throw Error(std::format("{}: too small for ELF", path));
	}
	void* m = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
	::close(fd);
	if (m == MAP_FAILED) {
		throw Error(std::format("mmap {}: {}", path, std::strerror(errno)));
	}
	std::unique_ptr<ElfFile> f(new ElfFile());
	f->base_ = static_cast<const char*>(m);
	f->size_ = static_cast<size_t>(st.st_size);

	auto eh = load<Elf64_Ehdr>(f->base_);
	if (std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0) {
		throw Error(std::format("{}: bad ELF magic", path));
	}
	if (eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_ident[EI_DATA] != ELFDATA2LSB) {
		throw Error(std::format("{}: only 64-bit little-endian ELF is supported", path));
	}
	f->machine_ = eh.e_machine;

	if (eh.e_phoff != 0 && eh.e_phentsize == sizeof(Elf64_Phdr)) {
		auto ph = f->bytes(eh.e_phoff, uint64_t{eh.e_phnum} * sizeof(Elf64_Phdr));
		for (uint16_t i = 0; i < eh.e_phnum; i++) {
			auto p = load<Elf64_Phdr>(ph.data() + i * sizeof(Elf64_Phdr));
			f->progs_.push_back(ElfProg{p.p_type, p.p_flags, p.p_offset, p.p_vaddr, p.p_filesz});
		}
	}
	if (eh.e_shoff != 0 && eh.e_shentsize == sizeof(Elf64_Shdr)) {
		uint64_t shnum = eh.e_shnum;
		auto first = load<Elf64_Shdr>(f->bytes(eh.e_shoff, sizeof(Elf64_Shdr)).data());
		if (shnum == 0) {
			shnum = first.sh_size;
		}
		uint32_t shstrndx = eh.e_shstrndx == SHN_XINDEX ? first.sh_link : eh.e_shstrndx;
		auto sh = f->bytes(eh.e_shoff, shnum * sizeof(Elf64_Shdr));
		std::vector<Elf64_Shdr> raw(shnum);
		for (uint64_t i = 0; i < shnum; i++) {
			raw[i] = load<Elf64_Shdr>(sh.data() + i * sizeof(Elf64_Shdr));
		}
		std::string_view names;
		if (shstrndx < shnum && raw[shstrndx].sh_type != SHT_NOBITS) {
			names = f->bytes(raw[shstrndx].sh_offset, raw[shstrndx].sh_size);
		}
		for (const auto& s : raw) {
			ElfSection out{"",          s.sh_type, s.sh_flags, s.sh_addr,
			               s.sh_offset, s.sh_size, s.sh_link,  s.sh_entsize};
			if (s.sh_name < names.size()) {
				auto n = names.substr(s.sh_name);
				out.name = std::string(n.substr(0, n.find('\0')));
			}
			f->sections_.push_back(std::move(out));
		}
	}
	return f;
}

ElfFile::~ElfFile() {
	if (base_) {
		::munmap(const_cast<char*>(base_), size_);
	}
}

std::string_view ElfFile::bytes(uint64_t off, uint64_t size) const {
	if (off > size_ || size > size_ - off) {
		throw Error("ELF: data out of file bounds");
	}
	return {base_ + off, static_cast<size_t>(size)};
}

const ElfSection* ElfFile::section(std::string_view name) const {
	for (const auto& s : sections_) {
		if (s.name == name) {
			return &s;
		}
	}
	return nullptr;
}

std::string_view ElfFile::data(const ElfSection& s) {
	if (s.type == SHT_NOBITS) {
		return {};
	}
	auto raw = bytes(s.offset, s.size);
	if (!(s.flags & SHF_COMPRESSED)) {
		return raw;
	}
	if (raw.size() < sizeof(Elf64_Chdr)) {
		throw Error("ELF: truncated compression header");
	}
	auto ch = load<Elf64_Chdr>(raw.data());
	if (ch.ch_type != ELFCOMPRESS_ZLIB) {
		throw Error(std::format("ELF: section {} uses unsupported compression {}", s.name, ch.ch_type));
	}
	if (ch.ch_size > (uint64_t{1} << 32)) {
		throw Error("ELF: compressed section too large");
	}
	auto out = std::make_unique<std::string>(ch.ch_size, '\0');
	uLongf n = static_cast<uLongf>(ch.ch_size);
	auto in = raw.substr(sizeof(Elf64_Chdr));
	if (uncompress(reinterpret_cast<Bytef*>(out->data()), &n, reinterpret_cast<const Bytef*>(in.data()),
	               static_cast<uLong>(in.size())) != Z_OK ||
	    n != ch.ch_size) {
		throw Error(std::format("ELF: cannot inflate section {}", s.name));
	}
	inflated_.push_back(std::move(out));
	return *inflated_.back();
}

std::string_view ElfFile::segment(const ElfProg& p) const {
	return bytes(p.offset, p.filesz);
}

std::vector<ElfSymbol> ElfFile::symbols(uint32_t type) {
	std::vector<ElfSymbol> out;
	for (const auto& s : sections_) {
		if (s.type != type || s.link >= sections_.size()) {
			continue;
		}
		auto tab = data(s);
		auto strs = data(sections_[s.link]);
		size_t n = tab.size() / sizeof(Elf64_Sym);
		out.reserve(out.size() + n);
		for (size_t i = 1; i < n; i++) {
			auto sym = load<Elf64_Sym>(tab.data() + i * sizeof(Elf64_Sym));
			std::string_view name;
			if (sym.st_name < strs.size()) {
				name = strs.substr(sym.st_name);
				name = name.substr(0, name.find('\0'));
			}
			out.push_back(ElfSymbol{name, sym.st_value, sym.st_size, sym.st_info});
		}
	}
	return out;
}

namespace {

constexpr uint32_t kNtGnuBuildId = 3;
constexpr uint32_t kMaxNoteSize = 1 << 16;

uint32_t align4(uint32_t n) {
	return (n + 3) & ~uint32_t{3};
}

std::string find_build_id(std::string_view r) {
	size_t pos = 0;
	while (pos + 12 <= r.size()) {
		auto name_sz = load<uint32_t>(r.data() + pos);
		auto desc_sz = load<uint32_t>(r.data() + pos + 4);
		auto type = load<uint32_t>(r.data() + pos + 8);
		pos += 12;
		if (name_sz > kMaxNoteSize || desc_sz > kMaxNoteSize) {
			return "";
		}
		size_t name_len = align4(name_sz), desc_len = align4(desc_sz);
		if (pos + name_len + desc_len > r.size()) {
			return "";
		}
		auto name = r.substr(pos, name_sz);
		auto desc = r.substr(pos + name_len, desc_sz);
		pos += name_len + desc_len;
		if (type == kNtGnuBuildId && name_sz == 4 && name.substr(0, 3) == "GNU" && desc_sz > 0) {
			return hex_encode(std::span(reinterpret_cast<const uint8_t*>(desc.data()), desc.size()));
		}
	}
	return "";
}

}  // namespace

std::string build_id(ElfFile& f) {
	// A note that cannot be read counts as absent, like a missing note.
	for (const auto& s : f.sections()) {
		if (s.type != SHT_NOTE) {
			continue;
		}
		try {
			if (auto id = find_build_id(f.data(s)); !id.empty()) {
				return id;
			}
		} catch (const Error&) {
		}
	}
	for (const auto& p : f.progs()) {
		if (p.type != PT_NOTE) {
			continue;
		}
		try {
			if (auto id = find_build_id(f.segment(p)); !id.empty()) {
				return id;
			}
		} catch (const Error&) {
		}
	}
	return "";
}

}  // namespace etrace::symbol
