#include "agent/source/offcpu/uprobes.h"

#include <sys/stat.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

#include "platform/sync.h"

namespace fs = std::filesystem;

namespace etrace::agent::offcpu::uprobes {

namespace {

std::string file_key(const std::string& path) {
	struct stat st{};
	if (::stat(path.c_str(), &st) != 0) {
		return path;
	}
	return std::format("{}:{}", st.st_dev, st.st_ino);
}

}  // namespace

std::vector<std::string> match_maps(std::istream& in, const std::vector<std::string>& patterns) {
	std::vector<std::string> out;
	std::set<std::string> seen;
	for (std::string line; std::getline(in, line);) {
		std::istringstream ls(line);
		std::vector<std::string> f;
		for (std::string tok; ls >> tok;) {
			f.push_back(tok);
		}
		if (f.size() < 6 || f[1].find('x') == std::string::npos) {
			continue;
		}
		std::string path = f[5];
		for (size_t i = 6; i < f.size(); i++) {
			path += ' ' + f[i];
		}
		if (path.empty() || path[0] != '/') {
			continue;
		}
		auto base = fs::path(path).filename().string();
		for (const auto& p : patterns) {
			if (base.find(p) != std::string::npos) {
				if (seen.insert(path).second) {
					out.push_back(path);
				}
				break;
			}
		}
	}
	return out;
}

std::vector<Target> Scanner::scan() const {
	std::set<std::string> seen;
	std::vector<Target> out;
	auto add = [&](const std::string& key, const std::string& path) {
		if (seen.insert(key).second) {
			out.push_back(Target{key, path});
		}
	};
	for (const auto& p : paths) {
		add(file_key(p), p);
	}
	if (patterns.empty()) {
		return out;
	}
	std::error_code ec;
	for (const auto& e : fs::directory_iterator(proc_root, ec)) {
		auto name = e.path().filename().string();
		if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos || name == "0") {
			continue;
		}
		std::ifstream in(e.path() / "maps");
		if (!in) {
			continue;
		}
		for (const auto& path : match_maps(in, patterns)) {
			auto full = (e.path() / "root" / fs::path(path).relative_path()).string();
			add(file_key(full), full);
		}
	}
	return out;
}

ProbeFn single(std::string symbol, std::string prog) {
	return [symbol = std::move(symbol), prog = std::move(prog)](const std::string&) {
		return std::vector<Probe>{{prog, symbol, false, 0}};
	};
}

Manager::Manager(Scanner sc, ProbeFn probes, std::string name, BpfObject& obj, log::Logger log)
    : scanner_(std::move(sc)), probes_(std::move(probes)), name_(std::move(name)), obj_(obj), log_(std::move(log)) {}

void Manager::run(std::stop_token st, Duration interval) {
	do {
		sync();
	} while (sleep_for(st, interval));
}

// sync attaches the probes of new targets. A target is not retried while it
// stays mapped once none of its probes attached; probes that fail in a
// target where others attached are logged and skipped.
void Manager::sync() {
	auto targets = scanner_.scan();
	std::set<std::string> live;
	std::lock_guard lock(mu_);
	for (const auto& t : targets) {
		live.insert(t.key);
		if (links_.contains(t.key) || failed_.contains(t.key)) {
			continue;
		}
		std::vector<std::unique_ptr<Link>> links;
		try {
			for (const auto& p : probes_(t.path)) {
				try {
					links.push_back(
					        obj_.attach_uprobe(p.prog, t.path, p.symbol, p.retprobe, p.cookie));
				} catch (const Error& e) {
					log_.warn("uprobe failed", {{"probes", name_}, {"path", t.path}, {"err", e}});
				}
			}
		} catch (const std::exception& e) {
			log_.warn("uprobe target unreadable", {{"probes", name_}, {"path", t.path}, {"err", e}});
		}
		if (links.empty()) {
			failed_.insert(t.key);
			continue;
		}
		log_.info("uprobes attached", {{"probes", name_}, {"path", t.path}, {"count", links.size()}});
		links_[t.key] = std::move(links);
	}
	std::erase_if(links_, [&](const auto& kv) { return !live.contains(kv.first); });
	std::erase_if(failed_, [&](const auto& k) { return !live.contains(k); });
}

size_t Manager::attached() {
	std::lock_guard lock(mu_);
	return links_.size();
}

void Manager::close() {
	std::lock_guard lock(mu_);
	links_.clear();
}

}  // namespace etrace::agent::offcpu::uprobes
