#pragma once

#include <ostream>
#include <stop_token>
#include <string>
#include <vector>

#include "agent/plugin.h"
#include "platform/time.h"

namespace etrace::app::doctor {

struct Options {
	Duration duration = 30s;
	Duration interval = 5s;
	uint32_t pid = 0;
	std::string comm;
	Duration min_block = 1ms;
	Duration slow = 20ms;
	std::string unwind = "auto";
	int depth = 4;
	int top = 20;
	std::string out = "etrace-report.html";
	std::string pprof;
	std::vector<std::string> debuginfod;
	std::string proc_root = "/proc";
	bool verbose = false;
};

// parse_flags throws flags::HelpRequested for -h and Error for invalid
// flags or values.
Options parse_flags(const std::vector<std::string>& args, std::ostream& err);

// processors builds the container and symbolize processors of a doctor run.
etrace::agent::ProcessorChain processors(const Options& o, const etrace::agent::Deps& deps);

// run checks the host, records, and writes the text summary to out and the
// HTML report to o.out. Stopping st ends recording early and still produces
// the report.
void run(std::stop_token st, const Options& o, std::ostream& out, std::ostream& err);

}  // namespace etrace::app::doctor
