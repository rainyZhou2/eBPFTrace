#include "platform/config.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace etrace::config {

std::string expand_env(std::string_view raw) {
	std::string out;
	out.reserve(raw.size());
	auto is_name = [](char c) { return c == '_' || std::isalnum(static_cast<unsigned char>(c)); };
	for (size_t i = 0; i < raw.size(); i++) {
		if (raw[i] != '$' || i + 1 >= raw.size()) {
			out += raw[i];
			continue;
		}
		std::string name;
		size_t end;
		if (raw[i + 1] == '{') {
			end = raw.find('}', i + 2);
			if (end == std::string_view::npos) {
				out += raw.substr(i);
				break;
			}
			name = raw.substr(i + 2, end - i - 2);
		} else if (is_name(raw[i + 1])) {
			end = i + 1;
			while (end < raw.size() && is_name(raw[end])) {
				end++;
			}
			name = raw.substr(i + 1, end - i - 1);
			end--;
		} else {
			out += raw[i];
			continue;
		}
		if (const char* v = std::getenv(name.c_str())) {
			out += v;
		}
		i = end;
	}
	return out;
}

Fields::Fields(const YAML::Node& node, std::string path) : node_(node), path_(std::move(path)) {}

void Fields::done() const {
	if (!node_.IsMap()) {
		return;
	}
	for (const auto& kv : node_) {
		auto key = kv.first.Scalar();
		if (!seen_.contains(key)) {
			fail(path_.empty() ? key : path_ + "." + key, "field not found in type");
		}
	}
}

void fail(const std::string& path, std::string_view what) {
	if (path.empty()) {
		throw Error(std::string(what));
	}
	throw Error(std::format("{}: {}", path, what));
}

void decode_fields(Fields& f, Plugin& p) {
	f("driver", p.driver)("options", p.options);
}

YAML::Node parse_text(std::string_view raw) {
	return YAML::Load(expand_env(raw));
}

std::string read_file(const std::string& path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		throw Error(std::format("read config: open {}: {}", path, std::strerror(errno)));
	}
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

}  // namespace etrace::config
