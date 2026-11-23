#include "query/flame.h"

#include <algorithm>

namespace etrace::query::flame {

void Node::add(const std::vector<std::string>& path, uint64_t v) {
	value += v;
	Node* cur = this;
	for (const auto& name : path) {
		auto it = cur->index_.find(name);
		Node* child;
		if (it == cur->index_.end()) {
			cur->children.push_back(std::make_unique<Node>(name));
			child = cur->children.back().get();
			cur->index_.emplace(name, child);
		} else {
			child = it->second;
		}
		child->value += v;
		cur = child;
	}
}

void Node::sort() {
	std::stable_sort(children.begin(), children.end(),
	                 [](const auto& a, const auto& b) { return a->value > b->value; });
	for (auto& c : children) {
		c->sort();
	}
}

nlohmann::ordered_json to_json(const Node& n) {
	nlohmann::ordered_json j;
	j["name"] = n.name;
	j["value"] = n.value;
	if (!n.children.empty()) {
		auto& arr = j["children"] = nlohmann::ordered_json::array();
		for (const auto& c : n.children) {
			arr.push_back(to_json(*c));
		}
	}
	return j;
}

void write_collapsed(std::ostream& out, const std::vector<Line>& lines) {
	std::string b;
	for (const auto& l : lines) {
		b.clear();
		for (size_t i = 0; i < l.path.size(); i++) {
			if (i > 0) {
				b += ';';
			}
			for (char c : l.path[i]) {
				b += c == ';' ? ':' : c;
			}
		}
		b += ' ';
		b += std::to_string(l.value);
		b += '\n';
		out << b;
	}
}

std::vector<std::string> reverse(const std::vector<std::string>& s) {
	return {s.rbegin(), s.rend()};
}

}  // namespace etrace::query::flame
