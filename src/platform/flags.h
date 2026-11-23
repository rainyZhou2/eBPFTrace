#pragma once

#include <cstdint>
#include <functional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "platform/error.h"
#include "platform/time.h"

namespace etrace::flags {

// HelpRequested is thrown for -h/-help after the usage was printed.
class HelpRequested : public Error {
public:
	HelpRequested() : Error("help requested") {}
};

// FlagSet parses command lines the way Go's flag package does: -name value,
// -name=value or --name, with boolean flags taking no value.
class FlagSet {
public:
	FlagSet(std::string name, std::ostream& err) : name_(std::move(name)), err_(err) {}

	void string(std::string_view name, std::string& out, std::string_view usage);
	void boolean(std::string_view name, bool& out, std::string_view usage);
	void duration(std::string_view name, Duration& out, std::string_view usage);
	void integer(std::string_view name, int& out, std::string_view usage);
	void unsigned32(std::string_view name, uint32_t& out, std::string_view usage);

	// parse throws Error (after printing it and the usage) on bad input and
	// HelpRequested for -h. It returns the remaining positional arguments.
	std::vector<std::string> parse(const std::vector<std::string>& args);
	void usage() const;

private:
	struct Flag {
		std::string name;
		std::string usage;
		std::string def;
		bool is_bool;
		std::function<void(std::string_view)> set;
	};
	void add(std::string_view name, std::string_view usage, std::string def, bool is_bool,
	         std::function<void(std::string_view)> set);

	std::string name_;
	std::ostream& err_;
	std::vector<Flag> flags_;
};

}  // namespace etrace::flags
