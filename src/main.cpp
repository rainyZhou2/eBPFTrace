#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "app/agent.h"
#include "app/collector.h"
#include "app/common.h"
#include "app/doctor.h"
#include "app/query.h"
#include "platform/config.h"
#include "platform/flags.h"
#include "platform/lifecycle.h"

namespace {

using namespace etrace;

void usage() {
	std::cerr << "usage: etrace <command> [-config path]\n"
	          << "  agent      run the node agent\n"
	          << "  collector  run the ingest collector\n"
	          << "  query      run the query API\n"
	          << "  doctor     record this host for a while and write a report (no backend needed)\n"
	          << "  version    print version\n";
}

template <class Config, class Run>
int serve(const std::string& name, const std::vector<std::string>& args, Run run) {
	std::string path = "/etc/etrace/" + name + ".yaml";
	flags::FlagSet fs(name, std::cerr);
	fs.string("config", path, "config file path");
	try {
		fs.parse(args);
	} catch (const flags::HelpRequested&) {
		return 0;
	} catch (const Error&) {
		return 2;
	}
	try {
		auto cfg = config::load<Config>(path);
		lifecycle::SignalWatcher signals;
		run(signals.token(), cfg);
	} catch (const std::exception& e) {
		std::cerr << "etrace " << name << ": " << e.what() << '\n';
		return 1;
	}
	return 0;
}

int doctor(const std::vector<std::string>& args) {
	app::doctor::Options o;
	try {
		o = app::doctor::parse_flags(args, std::cerr);
	} catch (const flags::HelpRequested&) {
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "etrace doctor: " << e.what() << '\n';
		return 2;
	}
	try {
		lifecycle::SignalWatcher signals;
		app::doctor::run(signals.token(), o, std::cout, std::cerr);
	} catch (const std::exception& e) {
		std::cerr << "etrace doctor: " << e.what() << '\n';
		return 1;
	}
	return 0;
}

}  // namespace

int main(int argc, char** argv) {
	if (argc < 2) {
		usage();
		return 2;
	}
	std::string name = argv[1];
	std::vector<std::string> args(argv + 2, argv + argc);
	if (name == "version") {
		std::cout << app::version << '\n';
		return 0;
	}
	if (name == "doctor") {
		return doctor(args);
	}
	if (name == "agent") {
		return serve<app::agent::Config>(name, args, app::agent::run);
	}
	if (name == "collector") {
		return serve<app::collector::Config>(name, args, app::collector::run);
	}
	if (name == "query") {
		return serve<app::query::Config>(name, args, app::query::run);
	}
	usage();
	return 2;
}
