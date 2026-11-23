#include "platform/flags.h"

#include <charconv>
#include <format>

namespace etrace::flags {

void FlagSet::add(std::string_view name, std::string_view usage, std::string def, bool is_bool,
                  std::function<void(std::string_view)> set) {
	flags_.push_back(Flag{std::string(name), std::string(usage), std::move(def), is_bool, std::move(set)});
}

void FlagSet::string(std::string_view name, std::string& out, std::string_view usage) {
	add(name, usage, out.empty() ? "" : std::format("\"{}\"", out), false, [&out](std::string_view v) { out = v; });
}

void FlagSet::boolean(std::string_view name, bool& out, std::string_view usage) {
	add(name, usage, "", true, [&out](std::string_view v) {
		if (v == "1" || v == "t" || v == "T" || v == "true" || v == "TRUE" || v == "True") {
			out = true;
		} else if (v == "0" || v == "f" || v == "F" || v == "false" || v == "FALSE" || v == "False") {
			out = false;
		} else {
			throw Error("parse error");
		}
	});
}

void FlagSet::duration(std::string_view name, Duration& out, std::string_view usage) {
	add(name, usage, format_duration(out), false, [&out](std::string_view v) { out = parse_duration(v); });
}

void FlagSet::integer(std::string_view name, int& out, std::string_view usage) {
	add(name, usage, std::to_string(out), false, [&out](std::string_view v) {
		int n;
		auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
		if (ec != std::errc() || p != v.data() + v.size()) {
			throw Error("parse error");
		}
		out = n;
	});
}

void FlagSet::unsigned32(std::string_view name, uint32_t& out, std::string_view usage) {
	add(name, usage, std::to_string(out), false, [&out](std::string_view v) {
		uint32_t n;
		auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
		if (ec != std::errc() || p != v.data() + v.size()) {
			throw Error("parse error");
		}
		out = n;
	});
}

void FlagSet::usage() const {
	err_ << "Usage of " << name_ << ":\n";
	for (const auto& f : flags_) {
		err_ << "  -" << f.name;
		if (!f.is_bool) {
			err_ << " value";
		}
		err_ << "\n    \t" << f.usage;
		if (!f.def.empty() && f.def != "0" && f.def != "0s") {
			err_ << " (default " << f.def << ")";
		}
		err_ << '\n';
	}
}

std::vector<std::string> FlagSet::parse(const std::vector<std::string>& args) {
	auto bad = [&](const std::string& msg) -> Error {
		err_ << msg << '\n';
		usage();
		return Error(msg);
	};
	size_t i = 0;
	for (; i < args.size(); i++) {
		std::string_view a = args[i];
		if (a.size() < 2 || a[0] != '-') {
			break;
		}
		if (a == "--") {
			i++;
			break;
		}
		a.remove_prefix(a[1] == '-' ? 2 : 1);
		std::string_view name = a, value;
		bool has_value = false;
		if (auto eq = a.find('='); eq != std::string_view::npos) {
			name = a.substr(0, eq);
			value = a.substr(eq + 1);
			has_value = true;
		}
		if (name == "h" || name == "help") {
			usage();
			throw HelpRequested();
		}
		Flag* flag = nullptr;
		for (auto& f : flags_) {
			if (f.name == name) {
				flag = &f;
			}
		}
		if (!flag) {
			throw bad(std::format("flag provided but not defined: -{}", name));
		}
		if (flag->is_bool && !has_value) {
			value = "true";
		} else if (!has_value) {
			if (i + 1 >= args.size()) {
				throw bad(std::format("flag needs an argument: -{}", name));
			}
			value = args[++i];
		}
		try {
			flag->set(value);
		} catch (const Error& e) {
			throw bad(std::format("invalid value \"{}\" for flag -{}: {}", value, name, e.what()));
		}
	}
	return {args.begin() + static_cast<std::ptrdiff_t>(i), args.end()};
}

}  // namespace etrace::flags
