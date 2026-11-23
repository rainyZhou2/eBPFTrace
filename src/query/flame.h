#pragma once

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace etrace::query::flame {

struct Node {
	std::string name;
	uint64_t value = 0;
	std::vector<std::unique_ptr<Node>> children;

	explicit Node(std::string n) : name(std::move(n)) {}
	static std::unique_ptr<Node> root() { return std::make_unique<Node>("root"); }

	// add inserts a path ordered from the outermost frame to the innermost.
	void add(const std::vector<std::string>& path, uint64_t v);
	// sort orders children by value, largest first, recursively.
	void sort();

private:
	std::unordered_map<std::string, Node*> index_;
};

// to_json renders {"name","value","children"} with children omitted when
// empty.
nlohmann::ordered_json to_json(const Node& n);

struct Line {
	std::vector<std::string> path;
	uint64_t value = 0;
};

// write_collapsed emits one "frame;frame;... value" line per path, the
// input format of flamegraph.pl and speedscope.
void write_collapsed(std::ostream& out, const std::vector<Line>& lines);

std::vector<std::string> reverse(const std::vector<std::string>& s);

}  // namespace etrace::query::flame
