#include "platform/ratelimit.h"

#include <algorithm>
#include <utility>

namespace etrace::ratelimit {

void decode_fields(config::Fields& f, Policy& p) {
	f("enabled", p.enabled)("rate", p.rate)("burst", p.burst)("idle_ttl", p.idle_ttl);
}

void Policy::validate() const {
	if (!enabled) {
		return;
	}
	if (!(rate > 0)) {
		throw Error("rate must be positive");
	}
	if (!(burst >= 1)) {
		throw Error("burst must be at least 1");
	}
	if (idle_ttl <= Duration(0)) {
		throw Error("idle_ttl must be positive");
	}
}

Limiter::Limiter(Policy p, Clock clock) : policy_(std::move(p)), clock_(std::move(clock)) {
	policy_.validate();
}

bool Limiter::allow(std::string_view key) {
	if (!policy_.enabled) {
		return true;
	}
	auto now = clock_();
	std::lock_guard lock(mu_);
	auto [it, inserted] = buckets_.try_emplace(std::string(key), Bucket{policy_.burst, now});
	auto& b = it->second;
	if (inserted) {
		b.tokens = policy_.burst;
	}
	double elapsed = std::chrono::duration<double>(now - b.updated).count();
	b.tokens = std::min(policy_.burst, b.tokens + elapsed * policy_.rate);
	b.updated = now;
	if (b.tokens < 1.0) {
		return false;
	}
	b.tokens -= 1.0;
	if (++calls_ % 1024 == 0) {
		auto ttl = policy_.idle_ttl;
		std::erase_if(buckets_, [&](const auto& entry) { return now - entry.second.updated > ttl; });
	}
	return true;
}

}  // namespace etrace::ratelimit
