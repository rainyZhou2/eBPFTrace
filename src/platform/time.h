#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace etrace {

using Duration = std::chrono::nanoseconds;
using Time = std::chrono::sys_time<std::chrono::nanoseconds>;

using namespace std::chrono_literals;

Time now();

inline int64_t unix_nano(Time t) {
	return t.time_since_epoch().count();
}
inline Time from_unix_nano(int64_t ns) {
	return Time(Duration(ns));
}
inline Time from_unix_micro(int64_t us) {
	return Time(std::chrono::microseconds(us));
}
inline Time from_unix(int64_t s) {
	return Time(std::chrono::seconds(s));
}
// The zero Time stands for "unset", like Go's time.Time{}.
inline bool is_zero(Time t) {
	return t.time_since_epoch().count() == 0;
}

// format_rfc3339nano renders t in UTC with trailing zeros of the fraction
// removed, e.g. 2024-05-01T10:00:00.25Z.
std::string format_rfc3339nano(Time t);
// parse_rfc3339 accepts YYYY-MM-DDTHH:MM:SS[.frac](Z|+hh:mm|-hh:mm).
std::optional<Time> parse_rfc3339(std::string_view s);
// format_local renders t in the local time zone with a strftime format.
std::string format_local(Time t, const char* fmt);

// format_duration and parse_duration use Go's duration syntax ("1m30s",
// "250ms", "1.5h") because configuration files and query parameters do.
std::string format_duration(Duration d);
// parse_duration throws Error on malformed input or overflow.
Duration parse_duration(std::string_view s);

}  // namespace etrace
