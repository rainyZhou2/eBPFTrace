#pragma once

#include <stop_token>

#include "platform/config.h"
#include "platform/time.h"

namespace etrace::backoff {

struct Policy {
	Duration initial = 200ms;
	Duration max = 30s;
	double multiplier = 2;
	double jitter = 0.2;

	// delay grows exponentially with attempt up to max, then spreads by
	// +/- jitter so retrying agents do not synchronize.
	Duration delay(int attempt) const;
	// wait sleeps for delay(attempt) and returns false when stopped first.
	bool wait(std::stop_token st, int attempt) const;
};

void decode_fields(config::Fields& f, Policy& p);

}  // namespace etrace::backoff
