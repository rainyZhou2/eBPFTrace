#pragma once

#include <condition_variable>
#include <mutex>
#include <stop_token>

#include "platform/time.h"

namespace etrace {

// sleep_for waits for d or until stop is requested and reports whether the
// full duration elapsed.
inline bool sleep_for(std::stop_token st, Duration d) {
	std::mutex mu;
	std::condition_variable_any cv;
	std::unique_lock lock(mu);
	cv.wait_for(lock, st, d, [] { return false; });
	return !st.stop_requested();
}

}  // namespace etrace
