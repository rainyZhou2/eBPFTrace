#pragma once

#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "model/model.h"
#include "platform/telemetry.h"

namespace etrace::testing {

namespace fs = std::filesystem;

// TempDir creates a fresh directory and removes it with its contents.
class TempDir {
public:
	TempDir() {
		auto tpl = (fs::temp_directory_path() / "etrace-test-XXXXXX").string();
		std::vector<char> buf(tpl.begin(), tpl.end());
		buf.push_back('\0');
		if (!mkdtemp(buf.data())) {
			throw std::runtime_error("mkdtemp failed");
		}
		path_ = buf.data();
	}
	~TempDir() {
		std::error_code ec;
		fs::remove_all(path_, ec);
	}
	const std::string& path() const { return path_; }
	std::string operator/(const std::string& rel) const { return (fs::path(path_) / rel).string(); }

private:
	std::string path_;
};

inline void write_file(const std::string& path, const std::string& data) {
	fs::create_directories(fs::path(path).parent_path());
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out << data;
}

inline std::string read_file(const std::string& path) {
	std::ifstream in(path, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

inline model::Stack frames(std::initializer_list<const char*> names) {
	model::Stack s;
	for (const auto* n : names) {
		s.push_back(model::Frame{0, n, "", model::FrameKind::unknown});
	}
	return s;
}

// wait_for polls cond for up to 5s.
inline bool wait_for(const std::function<bool()>& cond) {
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!cond()) {
		if (std::chrono::steady_clock::now() > deadline) {
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return true;
}

// metric sums every series of a family, optionally matching one label.
inline double metric(const telemetry::Registry& reg, const std::string& name, const std::string& label = "",
                     const std::string& value = "") {
	double sum = 0;
	for (const auto& mf : reg.gather()) {
		if (mf.name != name) {
			continue;
		}
		for (const auto& m : mf.metric) {
			bool match = label.empty();
			for (const auto& l : m.label) {
				match = match || (l.name == label && l.value == value);
			}
			if (match) {
				sum += m.counter.value + m.gauge.value;
			}
		}
	}
	return sum;
}

struct TestSym {
	std::string name;
	uint64_t addr, size;
};

// write_elf writes a minimal 64-bit little-endian ELF with one executable
// PT_LOAD covering the first 64KiB, a GNU build-id note and, when syms is
// non-empty, a .symtab. Without syms it looks like a stripped binary.
inline void write_elf(const std::string& path, const std::vector<uint8_t>& build_id, const std::vector<TestSym>& syms) {
	std::string body;
	auto put32 = [](std::string& b, uint32_t v) { b.append(reinterpret_cast<const char*>(&v), 4); };
	auto put64 = [](std::string& b, uint64_t v) { b.append(reinterpret_cast<const char*>(&v), 8); };
	auto put16 = [](std::string& b, uint16_t v) { b.append(reinterpret_cast<const char*>(&v), 2); };
	constexpr size_t data_start = 64 + 56;

	size_t note_off = data_start + body.size();
	put32(body, 4);
	put32(body, static_cast<uint32_t>(build_id.size()));
	put32(body, 3);
	body.append("GNU\0", 4);
	body.append(reinterpret_cast<const char*>(build_id.data()), build_id.size());
	while (body.size() % 4) {
		body += '\0';
	}
	size_t note_size = data_start + body.size() - note_off;

	std::string strtab(1, '\0');
	std::string symtab(24, '\0');
	for (const auto& s : syms) {
		std::string ent;
		put32(ent, static_cast<uint32_t>(strtab.size()));
		ent += static_cast<char>(1 << 4 | 2);
		ent += '\0';
		put16(ent, 1);
		put64(ent, s.addr);
		put64(ent, s.size);
		symtab += ent;
		strtab += s.name;
		strtab += '\0';
	}
	while (body.size() % 8) {
		body += '\0';
	}
	size_t sym_off = data_start + body.size();
	body += symtab;
	size_t str_off = data_start + body.size();
	body += strtab;
	std::string shstr("\0.note.gnu.build-id\0.symtab\0.strtab\0.shstrtab\0", 46);
	size_t shstr_off = data_start + body.size();
	body += shstr;
	while (body.size() % 8) {
		body += '\0';
	}
	size_t sh_off = data_start + body.size();

	struct Shdr {
		uint32_t name, type;
		uint64_t flags, addr, off, size;
		uint32_t link, info;
		uint64_t align, entsize;
	};
	std::vector<Shdr> sections{{}, {1, 7, 2, note_off, note_off, note_size, 0, 0, 4, 0}};
	if (!syms.empty()) {
		sections.push_back({20, 2, 0, 0, sym_off, symtab.size(), 3, 1, 8, 24});
		sections.push_back({28, 3, 0, 0, str_off, strtab.size(), 0, 0, 1, 0});
	}
	sections.push_back({36, 3, 0, 0, shstr_off, shstr.size(), 0, 0, 1, 0});

	std::string out;
	const char ident[16] = {0x7f, 'E', 'L', 'F', 2, 1, 1};
	out.append(ident, 16);
	put16(out, 3);    // ET_DYN
	put16(out, 183);  // EM_AARCH64
	put32(out, 1);
	put64(out, 0);
	put64(out, 64);
	put64(out, sh_off);
	put32(out, 0);
	put16(out, 64);
	put16(out, 56);
	put16(out, 1);
	put16(out, 64);
	put16(out, static_cast<uint16_t>(sections.size()));
	put16(out, static_cast<uint16_t>(sections.size() - 1));
	put32(out, 1);      // PT_LOAD
	put32(out, 1 | 4);  // PF_X | PF_R
	put64(out, 0);
	put64(out, 0);
	put64(out, 0);
	put64(out, 0x10000);
	put64(out, 0x10000);
	put64(out, 0x1000);
	out += body;
	for (const auto& s : sections) {
		put32(out, s.name);
		put32(out, s.type);
		put64(out, s.flags);
		put64(out, s.addr);
		put64(out, s.off);
		put64(out, s.size);
		put32(out, s.link);
		put32(out, s.info);
		put64(out, s.align);
		put64(out, s.entsize);
	}
	write_file(path, out);
}

inline std::string scenario_dir() {
	return std::string(ETRACE_SOURCE_DIR) + "/test/scenario";
}

}  // namespace etrace::testing
