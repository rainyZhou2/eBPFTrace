#include "app/doctor.h"

#include <unistd.h>

#include <format>
#include <fstream>

#include "agent/processor/container.h"
#include "agent/processor/symbolize.h"
#include "agent/source/offcpu/source.h"
#include "app/common.h"
#include "doctor/checks.h"
#include "doctor/report.h"
#include "doctor/session.h"
#include "platform/flags.h"
#include "query/service.h"
#include "storage/memory/memory.h"

namespace etrace::app::doctor {

namespace {

std::string target(const Options& o) {
	std::vector<std::string> parts;
	if (o.pid != 0) {
		parts.push_back(std::format("pid {}", o.pid));
	}
	if (!o.comm.empty()) {
		parts.push_back("comm " + o.comm);
	}
	if (parts.empty()) {
		return "all processes";
	}
	return parts.size() == 1 ? parts[0] : parts[0] + ", " + parts[1];
}

etrace::agent::offcpu::Options offcpu_options(const Options& o) {
	etrace::agent::offcpu::Options so;
	so.target_pid = o.pid;
	so.min_block = o.min_block;
	so.gpu.min_wait = o.min_block;
	so.slow_event = o.slow;
	so.chain_depth = o.depth;
	so.unwind.mode = o.unwind;
	return so;
}

void write_file(const std::string& path, std::string_view what, const std::function<void(std::ostream&)>& fill) {
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	if (!f) {
		throw Error(std::format("write {}: cannot open {}", what, path));
	}
	fill(f);
	f.close();
	if (!f) {
		throw Error(std::format("write {}: {} failed", what, path));
	}
}

}  // namespace

Options parse_flags(const std::vector<std::string>& args, std::ostream& err) {
	Options o;
	std::string debuginfod;
	flags::FlagSet fs("doctor", err);
	fs.duration("duration", o.duration, "how long to record");
	fs.unsigned32("pid", o.pid, "record only this process (default: every process)");
	fs.string("comm", o.comm, "report only threads with this command name");
	fs.duration("min-block", o.min_block, "ignore waits shorter than this");
	fs.duration("slow", o.slow, "list single waits at least this long");
	fs.string("unwind", o.unwind, "user stack unwinder: auto, dwarf or frame_pointer");
	fs.integer("depth", o.depth, "wakeup chain hops to keep (0-4)");
	fs.integer("top", o.top, "rows per report table");
	fs.string("o", o.out, "HTML report path, \"\" to skip");
	fs.string("pprof", o.pprof, "also write the off-CPU profile in pprof format to this path");
	fs.string("debuginfod", debuginfod,
	          "comma-separated debuginfod URLs for stripped binaries (sends build IDs to them)");
	fs.string("proc", o.proc_root, "procfs mount");
	fs.boolean("v", o.verbose, "log agent internals");
	fs.parse(args);
	for (size_t start = 0; !debuginfod.empty();) {
		auto comma = debuginfod.find(',', start);
		o.debuginfod.push_back(debuginfod.substr(start, comma - start));
		if (comma == std::string::npos) {
			break;
		}
		start = comma + 1;
	}
	if (o.duration < 1s) {
		throw Error("-duration must be at least 1s");
	}
	if (o.depth < 0 || o.depth > 4) {
		throw Error("-depth must be between 0 and 4");
	}
	if (o.top <= 0) {
		throw Error("-top must be positive");
	}
	if (o.slow < o.min_block) {
		throw Error("-slow must not be below -min-block");
	}
	return o;
}

etrace::agent::ProcessorChain processors(const Options& o, const etrace::agent::Deps& deps) {
	etrace::agent::symbolize::Options so;
	so.debuginfod.urls = o.debuginfod;
	so.validate();
	etrace::agent::ProcessorChain chain;
	chain.add(std::make_unique<etrace::agent::container::Processor>(o.proc_root,
	                                                                etrace::agent::container::Options{}));
	chain.add(etrace::agent::symbolize::Processor::from_options(so, deps));
	return chain;
}

void run(std::stop_token st, const Options& o, std::ostream& out, std::ostream& err) {
	auto log = log::make(err, log::Format::text, o.verbose ? log::Level::debug : log::Level::warn);
	auto host = etrace::doctor::probe_host(o.proc_root, o.pid);
	auto checks = etrace::doctor::preflight(host);
	if (etrace::doctor::failed(checks)) {
		for (const auto& c : checks) {
			err << std::format("[{}] {}: {}\n", etrace::doctor::to_string(c.status), c.name, c.detail);
		}
		throw Error("host cannot run the probes");
	}

	telemetry::Registry reg(false);
	etrace::agent::Deps deps{log, reg, o.proc_root};
	auto so = offcpu_options(o);
	so.validate();
	etrace::agent::offcpu::Source src(so, deps);
	auto chain = processors(o, deps);
	try {
		src.start();
	} catch (const std::exception& e) {
		throw Error(std::format("start probes: {}", e.what()));
	}
	err << std::format("recording off-CPU and GPU waits of {} for {}, Ctrl-C to stop early\n", target(o),
	                   format_duration(o.duration));
	char hostname[256] = {};
	gethostname(hostname, sizeof hostname - 1);
	storage::memory::Store store(storage::memory::Options{Duration(0)});
	etrace::doctor::Session s{src, chain, store, model::Resource{hostname, "", version, {}}};
	s.interval = o.interval;
	s.skip_pid = static_cast<uint32_t>(getpid());
	s.blocked = [&src] { return src.blocked(); };
	s.log = log;
	auto w = s.run(st, o.duration);
	src.close();

	storage::Filter filter;
	filter.comm = o.comm;
	filter.pid = o.pid;
	etrace::doctor::BuildRequest req{filter, o.depth, o.top, o.slow, std::move(w.blocked)};
	auto rep = etrace::doctor::build(store, req);
	rep.host = hostname;
	rep.kernel = host.kernel;
	rep.version = version;
	rep.target = target(o);
	rep.start = w.start;
	rep.end = w.end;
	rep.interrupted = w.interrupted;
	rep.checks = checks;
	auto rt = etrace::doctor::runtime_checks(reg.gather());
	rep.checks.insert(rep.checks.end(), rt.begin(), rt.end());

	etrace::doctor::write_text(out, rep);
	if (!o.pprof.empty()) {
		storage::Filter pf = filter;
		pf.from = w.start;
		pf.to = w.end;
		auto p = etrace::query::Service(store).profile(etrace::query::FlameRequest{pf, o.depth, 0});
		write_file(o.pprof, "pprof", [&](std::ostream& f) { f << p.gzip(); });
		out << "\npprof: " << o.pprof << "\n";
	}
	if (o.out.empty()) {
		return;
	}
	write_file(o.out, "report", [&](std::ostream& f) { etrace::doctor::write_html(f, rep); });
	out << "\nreport: " << o.out << "\n";
}

}  // namespace etrace::app::doctor
