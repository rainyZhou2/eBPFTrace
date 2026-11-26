#include "platform/config.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <functional>
#include <sstream>
#include <utility>

namespace etrace::config {

namespace {

constexpr std::string_view kTokenPrefix = "__etrace_env_";
constexpr std::string_view kTokenSuffix = "__";

using RefResolver =
        std::function<std::string(size_t index, std::string_view name, bool required, std::string_view message)>;

std::string substitute_env(std::string_view raw, const RefResolver& resolve) {
	std::string out;
	out.reserve(raw.size());
	auto is_name = [](char c) { return c == '_' || std::isalnum(static_cast<unsigned char>(c)); };
	size_t ref_index = 0;
	for (size_t i = 0; i < raw.size(); i++) {
		if (raw[i] != '$') {
			out += raw[i];
			continue;
		}
		if (i + 1 < raw.size() && raw[i + 1] == '$') {
			out += '$';
			i++;
			continue;
		}
		if (i + 1 >= raw.size()) {
			out += '$';
			continue;
		}
		std::string name;
		std::string required;
		bool is_required = false;
		size_t end;
		if (raw[i + 1] == '{') {
			end = raw.find('}', i + 2);
			if (end == std::string_view::npos) {
				throw Error("environment variable reference is missing '}'");
			}
			auto body = raw.substr(i + 2, end - i - 2);
			auto colon = body.find(":?");
			if (colon == std::string_view::npos) {
				name = body;
			} else {
				name = body.substr(0, colon);
				required = body.substr(colon + 2);
				is_required = true;
			}
			if (name.empty()) {
				throw Error("environment variable name is empty");
			}
		} else if (is_name(raw[i + 1])) {
			end = i + 1;
			while (end < raw.size() && is_name(raw[end])) {
				end++;
			}
			name = raw.substr(i + 1, end - i - 1);
			end--;
		} else {
			out += '$';
			continue;
		}
		out += resolve(ref_index++, name, is_required, required);
		i = end;
	}
	return out;
}

std::string token_for(size_t index) {
	return std::format("{}{}{}", kTokenPrefix, index, kTokenSuffix);
}

std::string expand_tokens(std::string_view raw, const std::vector<std::string>& values) {
	std::string out;
	out.reserve(raw.size());
	for (size_t i = 0; i < raw.size();) {
		if (raw.substr(i, kTokenPrefix.size()) == kTokenPrefix) {
			size_t pos = i + kTokenPrefix.size();
			size_t index = 0;
			bool digits = false;
			while (pos < raw.size() && std::isdigit(static_cast<unsigned char>(raw[pos]))) {
				index = index * 10 + static_cast<size_t>(raw[pos] - '0');
				digits = true;
				pos++;
			}
			if (digits && index < values.size() && raw.substr(pos, kTokenSuffix.size()) == kTokenSuffix) {
				out += values[index];
				i = pos + kTokenSuffix.size();
				continue;
			}
		}
		out += raw[i++];
	}
	return out;
}

struct Tokenized {
	std::string text;
	std::vector<std::string> values;
};

Tokenized tokenize_env(std::string_view raw) {
	Tokenized tokenized;
	tokenized.text =
	        substitute_env(raw, [&](size_t index, std::string_view name, bool required, std::string_view message) {
		        std::string key(name);
		        const char* value = std::getenv(key.c_str());
		        if (required && (!value || *value == '\0')) {
			        auto detail = message.empty() ? std::string() : ": " + std::string(message);
			        throw Error(std::format("environment variable {} is required{}", key, detail));
		        }
		        tokenized.values.push_back(value ? value : "");
		        return token_for(index);
	        });
	return tokenized;
}

}  // namespace

std::string expand_env(std::string_view raw) {
	return substitute_env(raw, [](size_t, std::string_view name, bool required, std::string_view message) {
		std::string key(name);
		const char* value = std::getenv(key.c_str());
		if (required && (!value || *value == '\0')) {
			auto detail = message.empty() ? std::string() : ": " + std::string(message);
			throw Error(std::format("environment variable {} is required{}", key, detail));
		}
		return std::string(value ? value : "");
	});
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

namespace {

void expand_node(YAML::Node node, const std::vector<std::string>& values, int depth) {
	if (depth > 64) {
		throw Error("config nesting is too deep");
	}
	if (node.IsScalar()) {
		auto raw = node.Scalar();
		auto expanded = expand_tokens(raw, values);
		if (expanded != raw) {
			node = expanded;
		}
		return;
	}
	if (node.IsSequence()) {
		for (auto element : node) {
			expand_node(element, values, depth + 1);
		}
		return;
	}
	if (node.IsMap()) {
		for (auto entry : node) {
			expand_node(entry.second, values, depth + 1);
		}
	}
}

}  // namespace

YAML::Node parse_text(std::string_view raw) {
	auto tokenized = tokenize_env(raw);
	YAML::Node root = YAML::Load(tokenized.text);
	expand_node(root, tokenized.values, 0);
	return root;
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
