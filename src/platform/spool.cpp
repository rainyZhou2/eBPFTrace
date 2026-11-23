#include "platform/spool.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

#include "platform/error.h"
#include "platform/time.h"

namespace fs = std::filesystem;

namespace etrace::spool {

namespace {

constexpr std::string_view kSuffix = ".rec";

[[noreturn]] void fail(std::string_view op, const std::string& path) {
	throw Error(std::format("spool: {} {}: {}", op, path, std::strerror(errno)));
}

void write_sync(const std::string& path, std::string_view data) {
	int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
	if (fd < 0) {
		fail("write", path);
	}
	size_t off = 0;
	while (off < data.size()) {
		ssize_t n = ::write(fd, data.data() + off, data.size() - off);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			int saved = errno;
			::close(fd);
			errno = saved;
			fail("write", path);
		}
		off += static_cast<size_t>(n);
	}
	if (::fsync(fd) != 0) {
		int saved = errno;
		::close(fd);
		errno = saved;
		fail("sync", path);
	}
	if (::close(fd) != 0) {
		fail("close", path);
	}
}

}  // namespace

Spool::Spool(std::string dir, int64_t max_bytes) : dir_(std::move(dir)), max_bytes_(max_bytes) {
	std::error_code ec;
	fs::create_directories(dir_, ec);
	if (ec) {
		throw Error(std::format("spool: mkdir {}: {}", dir_, ec.message()));
	}
	fs::permissions(dir_, fs::perms::owner_all, fs::perm_options::replace, ec);
	for (const auto& de : fs::directory_iterator(dir_, ec)) {
		auto name = de.path().filename().string();
		if (name.ends_with(".tmp")) {
			fs::remove(de.path(), ec);
			continue;
		}
		if (!name.ends_with(kSuffix)) {
			continue;
		}
		auto sz = fs::file_size(de.path(), ec);
		if (ec) {
			continue;
		}
		entries_.push_back(Entry{name, static_cast<int64_t>(sz)});
		size_ += static_cast<int64_t>(sz);
	}
	if (ec) {
		throw Error(std::format("spool: read dir {}: {}", dir_, ec.message()));
	}
	std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
}

void Spool::put(std::string_view data) {
	auto name = std::format("{:020}-{:06}{}", unix_nano(now()), (seq_.fetch_add(1) + 1) % 1000000, kSuffix);
	auto final_path = (fs::path(dir_) / name).string();
	auto tmp = final_path + ".tmp";
	try {
		write_sync(tmp, data);
	} catch (...) {
		::unlink(tmp.c_str());
		throw;
	}
	if (::rename(tmp.c_str(), final_path.c_str()) != 0) {
		int saved = errno;
		::unlink(tmp.c_str());
		errno = saved;
		fail("rename", tmp);
	}
	std::lock_guard lock(mu_);
	entries_.push_back(Entry{name, static_cast<int64_t>(data.size())});
	size_ += static_cast<int64_t>(data.size());
	while (max_bytes_ > 0 && size_ > max_bytes_ && entries_.size() > 1) {
		remove_locked(0);
		evicted_.fetch_add(1);
	}
}

std::optional<Record> Spool::peek() {
	std::lock_guard lock(mu_);
	while (!entries_.empty()) {
		const auto& e = entries_.front();
		std::ifstream in(fs::path(dir_) / e.name, std::ios::binary);
		if (in) {
			std::ostringstream ss;
			ss << in.rdbuf();
			if (in.good() || in.eof()) {
				return Record{e.name, ss.str()};
			}
		}
		remove_locked(0);
	}
	return std::nullopt;
}

void Spool::ack(const std::string& id) {
	std::lock_guard lock(mu_);
	for (size_t i = 0; i < entries_.size(); i++) {
		if (entries_[i].name == id) {
			remove_locked(i);
			return;
		}
	}
}

size_t Spool::size() const {
	std::lock_guard lock(mu_);
	return entries_.size();
}

int64_t Spool::bytes() const {
	std::lock_guard lock(mu_);
	return size_;
}

void Spool::remove_locked(size_t i) {
	auto& e = entries_[i];
	::unlink((fs::path(dir_) / e.name).c_str());
	size_ -= e.size;
	entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
}

}  // namespace etrace::spool
