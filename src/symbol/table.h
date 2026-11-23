#pragma once

#include <cstdint>
#include <istream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace etrace::symbol {

struct Symbol {
	uint64_t start = 0;
	uint64_t size = 0;
	std::string name;
};

// Table resolves addresses to the closest preceding symbol.
class Table {
public:
	Table() = default;
	explicit Table(std::vector<Symbol> syms);

	// lookup returns nullptr when addr precedes every symbol or lies past
	// the end of a sized symbol.
	const std::string* lookup(uint64_t addr) const;
	size_t size() const { return syms_.size(); }
	const std::vector<Symbol>& symbols() const { return syms_; }

private:
	std::vector<Symbol> syms_;
};

std::shared_ptr<const Table> parse_kallsyms(std::istream& in);
std::shared_ptr<const Table> load_kallsyms(const std::string& path);

struct Mapping {
	uint64_t start = 0;
	uint64_t end = 0;
	uint64_t offset = 0;
	std::string path;
	bool exec = false;
};

std::vector<Mapping> parse_maps(std::istream& in);
const Mapping* find_mapping(const std::vector<Mapping>& maps, uint64_t addr);

}  // namespace etrace::symbol
