#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "platform/config.h"

namespace etrace::ratelimit {

struct Policy {
	bool enabled = true;
	double rate = 20.0;
	double burst = 40.0;
	Duration idle_ttl = 10min;

	void validate() const;
};

void decode_fields(config::Fields& f, Policy& p);

// Limiter is a keyed token bucket. A key that has been idle for longer than
// idle_ttl is removed; keys are reclaimed during allow().
class Limiter {
public:
	using Clock = std::function<std::chrono::steady_clock::time_point()>;

	explicit Limiter(Policy p, Clock clock = std::chrono::steady_clock::now);

	// allow consumes one token for key and reports whether the request may
	// proceed. A disabled policy allows everything.
	bool allow(std::string_view key);

private:
	struct Bucket {
		double tokens = 0;
		std::chrono::steady_clock::time_point updated;
	};

	Policy policy_;
	Clock clock_;
	std::mutex mu_;
	std::unordered_map<std::string, Bucket> buckets_;
	size_t calls_ = 0;
};

}  // namespace etrace::ratelimit
