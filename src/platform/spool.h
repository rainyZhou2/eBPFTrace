#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

namespace etrace::spool {

struct Record {
	std::string id;
	std::string data;
};

// Spool is a bounded, crash-safe FIFO of opaque records on local disk. Each
// record is one file written via rename, so a crash never leaves a partial
// record visible. When the byte budget is exceeded the oldest records are
// evicted.
class Spool {
public:
	// Opening recovers records left by a previous run and removes partial
	// writes. max_bytes <= 0 disables the budget.
	Spool(std::string dir, int64_t max_bytes);

	void put(std::string_view data);
	// peek returns the oldest readable record, or nullopt when empty.
	std::optional<Record> peek();
	void ack(const std::string& id);

	size_t size() const;
	int64_t bytes() const;
	uint64_t evicted() const { return evicted_.load(); }

private:
	struct Entry {
		std::string name;
		int64_t size;
	};
	void remove_locked(size_t i);

	std::string dir_;
	int64_t max_bytes_;
	mutable std::mutex mu_;
	std::atomic<uint64_t> seq_{0};
	int64_t size_ = 0;
	std::deque<Entry> entries_;
	std::atomic<uint64_t> evicted_{0};
};

}  // namespace etrace::spool
