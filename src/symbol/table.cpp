#include "symbol/table.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>

#include "platform/error.h"

namespace etrace::symbol {

Table::Table(std::vector<Symbol> syms) : syms_(std::move(syms)) {
	std::stable_sort(syms_.begin(), syms_.end(),
	                 [](const Symbol& a, const Symbol& b) { return a.start < b.start; });
}

const std::string* Table::lookup(uint64_t addr) const {
	auto it = std::upper_bound(syms_.begin(), syms_.end(), addr,
	                           [](uint64_t a, const Symbol& s) { return a < s.start; });
	if (it == syms_.begin()) {
		return nullptr;
	}
	--it;
	if (it->size > 0 && addr >= it->start + it->size) {
		return nullptr;
	}
	return &it->name;
}

namespace {

bool parse_hex(std::string_view s, uint64_t& out) {
	auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out, 16);
	return ec == std::errc() && p == s.data() + s.size();
}

std::vector<std::string_view> fields(std::string_view line) {
	std::vector<std::string_view> out;
	size_t i = 0;
	while (i < line.size()) {
		while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
			i++;
		}
		size_t start = i;
		while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
			i++;
		}
		if (i > start) {
			out.push_back(line.substr(start, i - start));
		}
	}
	return out;
}

}  // namespace

std::shared_ptr<const Table> parse_kallsyms(std::istream& in) {
	std::vector<Symbol> syms;
	for (std::string line; std::getline(in, line);) {
		auto f = fields(line);
		if (f.size() < 3) {
			continue;
		}
		if (f[1] != "t" && f[1] != "T" && f[1] != "w" && f[1] != "W") {
			continue;
		}
		uint64_t addr;
		if (!parse_hex(f[0], addr) || addr == 0) {
			continue;
		}
		std::string name(f[2]);
		if (f.size() > 3) {
			name += ' ';
			name += f[3];
		}
		syms.push_back(Symbol{addr, 0, std::move(name)});
	}
	return std::make_shared<const Table>(std::move(syms));
}

std::shared_ptr<const Table> load_kallsyms(const std::string& path) {
	std::ifstream in(path);
	if (!in) {
		throw Error("open " + path);
	}
	return parse_kallsyms(in);
}

std::vector<Mapping> parse_maps(std::istream& in) {
	std::vector<Mapping> out;
	for (std::string line; std::getline(in, line);) {
		auto f = fields(line);
		if (f.size() < 5) {
			continue;
		}
		auto dash = f[0].find('-');
		if (dash == std::string_view::npos) {
			continue;
		}
		Mapping m;
		if (!parse_hex(f[0].substr(0, dash), m.start) || !parse_hex(f[0].substr(dash + 1), m.end) ||
		    !parse_hex(f[2], m.offset)) {
			continue;
		}
		m.exec = f[1].find('x') != std::string_view::npos;
		if (f.size() >= 6) {
			for (size_t i = 5; i < f.size(); i++) {
				if (i > 5) {
					m.path += ' ';
				}
				m.path += f[i];
			}
		}
		out.push_back(std::move(m));
	}
	return out;
}

const Mapping* find_mapping(const std::vector<Mapping>& maps, uint64_t addr) {
	for (const auto& m : maps) {
		if (addr >= m.start && addr < m.end) {
			return &m;
		}
	}
	return nullptr;
}

}  // namespace etrace::symbol
