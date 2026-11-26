#pragma once

#include <yaml-cpp/yaml.h>

#include <concepts>
#include <cstdint>
#include <format>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "platform/error.h"
#include "platform/time.h"

namespace etrace::config {

class Fields;

// A struct is decodable when it provides `void decode_fields(Fields&, T&)`
// found by argument-dependent lookup. Default member initializers are the
// defaults; a `validate()` member, when present, runs after decoding.
template <class T>
concept Decodable = requires(Fields& f, T& t) { decode_fields(f, t); };

template <class T>
concept Validatable = requires(T& t) { t.validate(); };

// expand_env replaces $VAR and ${VAR} with the environment value. ${VAR:?}
// fails when the variable is unset or empty; $$ is a literal dollar.
std::string expand_env(std::string_view raw);

template <class T>
void decode(const YAML::Node& n, T& out, const std::string& path);

// Fields decodes the keys of one YAML mapping and rejects keys nobody read.
class Fields {
public:
	Fields(const YAML::Node& node, std::string path);

	template <class T>
	Fields& operator()(std::string_view key, T& out) {
		std::string k(key);
		seen_.insert(k);
		if (node_.IsMap()) {
			if (auto v = node_[k]) {
				decode(v, out, path_.empty() ? k : path_ + "." + k);
			}
		}
		return *this;
	}

	void done() const;
	const std::string& path() const { return path_; }

private:
	YAML::Node node_;
	std::string path_;
	std::set<std::string> seen_;
};

[[noreturn]] void fail(const std::string& path, std::string_view what);

template <class T>
void validate(T& v, const std::string& path) {
	if constexpr (Validatable<T>) {
		try {
			v.validate();
		} catch (const Error& e) {
			if (path.empty()) {
				throw;
			}
			throw Error(std::format("{}: {}", path, e.what()));
		}
	}
}

template <class T>
void decode(const YAML::Node& n, T& out, const std::string& path) {
	if (n.IsNull()) {
		out = T{};
		return;
	}
	if constexpr (std::is_same_v<T, std::string>) {
		if (!n.IsScalar()) {
			fail(path, "expected a string");
		}
		out = n.Scalar();
	} else if constexpr (std::is_same_v<T, bool>) {
		bool v;
		if (!n.IsScalar() || !YAML::convert<bool>::decode(n, v)) {
			fail(path, "expected a boolean");
		}
		out = v;
	} else if constexpr (std::is_integral_v<T>) {
		T v;
		if (!n.IsScalar() || !YAML::convert<T>::decode(n, v)) {
			fail(path, "expected an integer");
		}
		out = v;
	} else if constexpr (std::is_floating_point_v<T>) {
		T v;
		if (!n.IsScalar() || !YAML::convert<T>::decode(n, v)) {
			fail(path, "expected a number");
		}
		out = v;
	} else if constexpr (std::is_same_v<T, Duration>) {
		if (!n.IsScalar()) {
			fail(path, "expected a duration");
		}
		int64_t ns;
		if (YAML::convert<int64_t>::decode(n, ns)) {
			out = Duration(ns);
			return;
		}
		try {
			out = parse_duration(n.Scalar());
		} catch (const Error& e) {
			fail(path, e.what());
		}
	} else if constexpr (requires {
		                     typename T::value_type;
		                     requires std::is_same_v<T, std::vector<typename T::value_type>>;
	                     }) {
		if (!n.IsSequence()) {
			fail(path, "expected a list");
		}
		T v;
		for (size_t i = 0; i < n.size(); i++) {
			typename T::value_type item{};
			decode(n[i], item, std::format("{}[{}]", path, i));
			v.push_back(std::move(item));
		}
		out = std::move(v);
	} else if constexpr (std::is_same_v<T, std::map<std::string, std::string>>) {
		if (!n.IsMap()) {
			fail(path, "expected a mapping");
		}
		T v;
		for (const auto& kv : n) {
			auto key = kv.first.Scalar();
			std::string val;
			decode(kv.second, val, path + "." + key);
			v[key] = std::move(val);
		}
		out = std::move(v);
	} else if constexpr (std::is_same_v<T, YAML::Node>) {
		out = YAML::Clone(n);
	} else if constexpr (Decodable<T>) {
		if (!n.IsMap()) {
			fail(path, "expected a mapping");
		}
		Fields f(n, path);
		decode_fields(f, out);
		f.done();
		validate(out, path);
	} else {
		static_assert(sizeof(T) == 0, "type has no YAML decoder");
	}
}

// Plugin selects a driver by name and keeps its options undecoded until the
// driver knows their type.
struct Plugin {
	std::string driver;
	YAML::Node options;

	// options_as decodes the options over T's defaults, rejecting unknown
	// fields, and validates the result.
	template <class T>
	T options_as() const {
		T out{};
		try {
			if (options && !options.IsNull()) {
				decode(options, out, "");
			} else {
				validate(out, "");
			}
		} catch (const Error& e) {
			throw Error(std::format("{} options: {}", driver, e.what()));
		}
		return out;
	}
};

void decode_fields(Fields& f, Plugin& p);

YAML::Node parse_text(std::string_view raw);

// parse decodes YAML text (after environment expansion) into T and
// validates it.
template <class T>
T parse(std::string_view raw) {
	T out{};
	try {
		auto root = parse_text(raw);
		if (root.IsNull()) {
			validate(out, "");
		} else {
			decode(root, out, "");
		}
	} catch (const YAML::Exception& e) {
		throw Error(std::format("decode config: {}", e.what()));
	} catch (const Error& e) {
		throw Error(std::format("invalid config: {}", e.what()));
	}
	return out;
}

std::string read_file(const std::string& path);

template <class T>
T load(const std::string& path) {
	return parse<T>(read_file(path));
}

}  // namespace etrace::config
