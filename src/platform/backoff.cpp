#include "platform/backoff.h"

#include <random>

#include "platform/sync.h"

namespace etrace::backoff {

Duration Policy::delay(int attempt) const {
	double d = static_cast<double>(initial.count());
	for (int i = 0; i < attempt; i++) {
		d *= multiplier;
		if (d >= static_cast<double>(max.count())) {
			d = static_cast<double>(max.count());
			break;
		}
	}
	if (jitter > 0) {
		thread_local std::mt19937_64 rng{std::random_device{}()};
		std::uniform_real_distribution<double> dist(-1.0, 1.0);
		d += d * jitter * dist(rng);
	}
	return Duration(static_cast<int64_t>(d));
}

bool Policy::wait(std::stop_token st, int attempt) const {
	return sleep_for(st, delay(attempt));
}

void decode_fields(config::Fields& f, Policy& p) {
	f("initial", p.initial)("max", p.max)("multiplier", p.multiplier)("jitter", p.jitter);
}

}  // namespace etrace::backoff
