#include "symbol/debuginfod.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <httplib.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>

#include "platform/error.h"
#include "platform/hex.h"
#include "platform/url.h"

namespace fs = std::filesystem;

namespace etrace::symbol {

bool valid_build_id(const std::string& id) {
	if (id.size() < 2 || id.size() > 128) {
		return false;
	}
	return std::all_of(id.begin(), id.end(),
	                   [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

void DebuginfodConfig::validate() const {
	if (urls.empty()) {
		return;
	}
	for (const auto& raw : urls) {
		if (!parse_http_url(raw)) {
			throw Error(std::format("debuginfod url \"{}\" must be an absolute http(s) URL", raw));
		}
	}
	if (cache_dir.empty()) {
		throw Error("debuginfod.cache_dir is required");
	}
	if (max_file_bytes <= 0 || cache_max_bytes < max_file_bytes) {
		throw Error("debuginfod: cache_max_bytes must be at least max_file_bytes > 0");
	}
	if (timeout <= Duration(0)) {
		throw Error("debuginfod.timeout must be positive");
	}
	if (workers <= 0 || queue_size <= 0) {
		throw Error("debuginfod: workers and queue_size must be positive");
	}
	if (retry_after <= Duration(0)) {
		throw Error("debuginfod.retry_after must be positive");
	}
}

void decode_fields(config::Fields& f, DebuginfodConfig& c) {
	f("urls", c.urls)("cache_dir", c.cache_dir)("cache_max_bytes", c.cache_max_bytes)(
	        "max_file_bytes", c.max_file_bytes)("timeout", c.timeout)("workers", c.workers)(
	        "queue_size", c.queue_size)("retry_after", c.retry_after);
}

Debuginfod::Debuginfod(DebuginfodConfig cfg, log::Logger log, telemetry::Registry& reg)
    : cfg_(std::move(cfg)),
      log_(log.with({{"component", "debuginfod"}})),
      failed_(4096, cfg_.retry_after),
      fetches_(reg.counter_family("etrace_debuginfod_fetches_total", "Debug file downloads by result.")),
      dropped_(reg.counter("etrace_debuginfod_queue_dropped_total",
                           "Download requests dropped because the queue was full.")) {
	std::error_code ec;
	fs::create_directories(cfg_.cache_dir, ec);
	if (ec) {
		throw Error(std::format("debuginfod cache: {}", ec.message()));
	}
	fs::permissions(cfg_.cache_dir, fs::perms::owner_all, fs::perm_options::replace, ec);
	for (const auto& de : fs::directory_iterator(cfg_.cache_dir, ec)) {
		if (de.path().extension() == ".tmp") {
			fs::remove(de.path(), ec);
		}
	}
	for (int i = 0; i < cfg_.workers; i++) {
		workers_.emplace_back([this](std::stop_token st) { work(st); });
	}
}

Debuginfod::~Debuginfod() {
	for (auto& w : workers_) {
		w.request_stop();
	}
	cv_.notify_all();
	workers_.clear();
}

std::string Debuginfod::path(const std::string& id) const {
	return (fs::path(cfg_.cache_dir) / (id + ".debug")).string();
}

std::optional<std::string> Debuginfod::find(const std::string& id) {
	if (!valid_build_id(id)) {
		return std::nullopt;
	}
	auto p = path(id);
	std::error_code ec;
	if (fs::exists(p, ec)) {
		fs::last_write_time(p, fs::file_time_type::clock::now(), ec);
		return p;
	}
	request(id);
	return std::nullopt;
}

void Debuginfod::request(const std::string& id) {
	if (failed_.get(id)) {
		return;
	}
	std::lock_guard lock(mu_);
	if (inflight_.contains(id)) {
		return;
	}
	if (queue_.size() >= static_cast<size_t>(cfg_.queue_size)) {
		dropped_.Increment();
		return;
	}
	queue_.push_back(id);
	inflight_.insert(id);
	cv_.notify_one();
}

void Debuginfod::work(std::stop_token st) {
	while (true) {
		std::string id;
		{
			std::unique_lock lock(mu_);
			if (!cv_.wait(lock, st, [this] { return !queue_.empty(); })) {
				return;
			}
			id = std::move(queue_.front());
			queue_.pop_front();
		}
		auto result = fetch(st, id);
		fetches_.Add({{"result", result}}).Increment();
		if (result != "ok" && !st.stop_requested()) {
			failed_.add(id, true);
		}
		std::lock_guard lock(mu_);
		inflight_.erase(id);
	}
}

// fetch tries each server in order and reports the outcome of the last
// attempt. Servers answer 404 for unknown build IDs, so trying the next one
// is the normal path rather than a retry.
std::string Debuginfod::fetch(std::stop_token st, const std::string& id) {
	std::string result = "not_found";
	for (const auto& base : cfg_.urls) {
		result = fetch_from(st, base, id);
		if (result == "ok" || st.stop_requested()) {
			return result;
		}
	}
	return result;
}

std::string Debuginfod::fetch_from(std::stop_token st, const std::string& base, const std::string& id) {
	auto url = parse_http_url(base);
	if (!url) {
		return "error";
	}
	httplib::Client cli(url->origin());
	auto t = std::chrono::duration_cast<std::chrono::microseconds>(cfg_.timeout);
	cli.set_connection_timeout(t);
	cli.set_read_timeout(t);
	cli.set_follow_location(true);
	auto target = url->path + "/buildid/" + id + "/debuginfo";
	auto deadline = std::chrono::steady_clock::now() + cfg_.timeout;

	auto tmp_tpl = (fs::path(cfg_.cache_dir) / (id + "-XXXXXX.tmp")).string();
	std::vector<char> tmpl(tmp_tpl.begin(), tmp_tpl.end());
	tmpl.push_back('\0');
	int fd = ::mkstemps(tmpl.data(), 4);
	if (fd < 0) {
		log_.warn("debuginfod cache write failed", {{"err", std::strerror(errno)}});
		return "error";
	}
	std::string tmp(tmpl.data());
	struct Cleanup {
		int fd;
		std::string path;
		~Cleanup() {
			if (fd >= 0) {
				::close(fd);
			}
			::unlink(path.c_str());
		}
	} cleanup{fd, tmp};

	int64_t written = 0;
	bool too_large = false, write_failed = false, expired = false;
	int status = 0;
	auto res = cli.Get(
	        target,
	        [&](const httplib::Response& r) {
		        status = r.status;
		        if (r.status != 200) {
			        return false;
		        }
		        if (r.has_header("Content-Length")) {
			        try {
				        if (std::stoll(r.get_header_value("Content-Length")) > cfg_.max_file_bytes) {
					        too_large = true;
					        return false;
				        }
			        } catch (const std::exception&) {
				        return false;
			        }
		        }
		        return true;
	        },
	        [&](const char* data, size_t n) {
		        if (st.stop_requested()) {
			        return false;
		        }
		        if (std::chrono::steady_clock::now() > deadline) {
			        expired = true;
			        return false;
		        }
		        if (written + static_cast<int64_t>(n) > cfg_.max_file_bytes) {
			        too_large = true;
			        return false;
		        }
		        size_t off = 0;
		        while (off < n) {
			        ssize_t w = ::write(cleanup.fd, data + off, n - off);
			        if (w < 0) {
				        if (errno == EINTR) {
					        continue;
				        }
				        write_failed = true;
				        return false;
			        }
			        off += static_cast<size_t>(w);
		        }
		        written += static_cast<int64_t>(n);
		        return true;
	        });
	if (status == 404) {
		return "not_found";
	}
	if (too_large) {
		return "too_large";
	}
	if (write_failed) {
		log_.warn("debuginfod cache write failed", {{"err", std::strerror(errno)}});
		return "error";
	}
	if (status != 0 && status != 200) {
		log_.debug("debuginfod unexpected status", {{"url", base}, {"status", status}});
		return "error";
	}
	if (!res || expired || st.stop_requested()) {
		log_.debug("debuginfod request failed",
		           {{"url", base}, {"err", expired ? "timeout" : httplib::to_string(res.error())}});
		return "error";
	}
	if (::close(cleanup.fd) != 0) {
		cleanup.fd = -1;
		return "error";
	}
	cleanup.fd = -1;
	try {
		auto f = ElfFile::open(tmp);
		if (build_id(*f) != id) {
			return "invalid";
		}
	} catch (const Error&) {
		return "invalid";
	}
	if (::rename(tmp.c_str(), path(id).c_str()) != 0) {
		log_.warn("debuginfod cache write failed", {{"err", std::strerror(errno)}});
		return "error";
	}
	prune();
	return "ok";
}

// prune deletes the least recently used files until the cache fits its
// budget. find refreshes the modification time on every hit.
void Debuginfod::prune() {
	std::lock_guard lock(prune_mu_);
	struct File {
		fs::path path;
		int64_t size;
		fs::file_time_type mtime;
	};
	std::vector<File> files;
	int64_t total = 0;
	std::error_code ec;
	for (const auto& de : fs::directory_iterator(cfg_.cache_dir, ec)) {
		if (de.path().extension() != ".debug") {
			continue;
		}
		auto sz = de.file_size(ec);
		if (ec) {
			continue;
		}
		auto mt = de.last_write_time(ec);
		if (ec) {
			continue;
		}
		files.push_back(File{de.path(), static_cast<int64_t>(sz), mt});
		total += static_cast<int64_t>(sz);
	}
	std::sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.mtime < b.mtime; });
	for (const auto& f : files) {
		if (total <= cfg_.cache_max_bytes) {
			return;
		}
		if (fs::remove(f.path, ec)) {
			total -= f.size;
		}
	}
}

}  // namespace etrace::symbol
