#include "platform/time.h"

#include <ctime>
#include <format>
#include <limits>

#include "platform/error.h"

namespace etrace {

Time now() {
	return std::chrono::time_point_cast<Duration>(std::chrono::system_clock::now());
}

std::string format_rfc3339nano(Time t) {
	auto day = std::chrono::floor<std::chrono::days>(t);
	std::chrono::year_month_day ymd{day};
	std::chrono::hh_mm_ss tod{t - day};
	auto out = std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}", static_cast<int>(ymd.year()),
	                       static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()),
	                       tod.hours().count(), tod.minutes().count(), tod.seconds().count());
	auto frac = tod.subseconds().count();
	if (frac != 0) {
		auto digits = std::format("{:09}", frac);
		digits.erase(digits.find_last_not_of('0') + 1);
		out += '.';
		out += digits;
	}
	out += 'Z';
	return out;
}

namespace {

bool digits(std::string_view s, size_t pos, size_t n, int& out) {
	if (pos + n > s.size()) {
		return false;
	}
	int v = 0;
	for (size_t i = pos; i < pos + n; i++) {
		if (s[i] < '0' || s[i] > '9') {
			return false;
		}
		v = v * 10 + (s[i] - '0');
	}
	out = v;
	return true;
}

}  // namespace

std::optional<Time> parse_rfc3339(std::string_view s) {
	int y, mo, d, h, mi, sec;
	if (!digits(s, 0, 4, y) || s.size() < 19 || s[4] != '-' || !digits(s, 5, 2, mo) || s[7] != '-' ||
	    !digits(s, 8, 2, d) || (s[10] != 'T' && s[10] != 't') || !digits(s, 11, 2, h) || s[13] != ':' ||
	    !digits(s, 14, 2, mi) || s[16] != ':' || !digits(s, 17, 2, sec)) {
		return std::nullopt;
	}
	std::chrono::year_month_day ymd{std::chrono::year(y), std::chrono::month(mo), std::chrono::day(d)};
	if (!ymd.ok() || h > 23 || mi > 59 || sec > 59) {
		return std::nullopt;
	}
	size_t pos = 19;
	int64_t nanos = 0;
	if (pos < s.size() && s[pos] == '.') {
		pos++;
		size_t start = pos;
		int64_t scale = 100000000;
		while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
			nanos += (s[pos] - '0') * scale;
			scale /= 10;
			pos++;
		}
		if (pos == start) {
			return std::nullopt;
		}
	}
	int offset_min = 0;
	if (pos < s.size() && (s[pos] == 'Z' || s[pos] == 'z')) {
		pos++;
	} else if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) {
		int oh, om;
		if (!digits(s, pos + 1, 2, oh) || pos + 3 >= s.size() || s[pos + 3] != ':' ||
		    !digits(s, pos + 4, 2, om) || oh > 23 || om > 59) {
			return std::nullopt;
		}
		offset_min = (oh * 60 + om) * (s[pos] == '-' ? -1 : 1);
		pos += 6;
	} else {
		return std::nullopt;
	}
	if (pos != s.size()) {
		return std::nullopt;
	}
	Time t = std::chrono::sys_days(ymd) + std::chrono::hours(h) + std::chrono::minutes(mi) +
	         std::chrono::seconds(sec) + Duration(nanos);
	return t - std::chrono::minutes(offset_min);
}

std::string format_local(Time t, const char* fmt) {
	std::time_t secs = std::chrono::system_clock::to_time_t(std::chrono::time_point_cast<std::chrono::seconds>(t));
	std::tm tm{};
	localtime_r(&secs, &tm);
	char buf[64];
	size_t n = std::strftime(buf, sizeof buf, fmt, &tm);
	return std::string(buf, n);
}

namespace {

// fmt_frac appends the fraction of v/10^prec without trailing zeros and
// returns the integer part, mirroring Go's time.Duration formatting.
uint64_t fmt_frac(std::string& rev, uint64_t v, int prec) {
	bool print = false;
	for (int i = 0; i < prec; i++) {
		uint64_t digit = v % 10;
		print = print || digit != 0;
		if (print) {
			rev += static_cast<char>('0' + digit);
		}
		v /= 10;
	}
	if (print) {
		rev += '.';
	}
	return v;
}

void fmt_int(std::string& rev, uint64_t v) {
	if (v == 0) {
		rev += '0';
		return;
	}
	while (v > 0) {
		rev += static_cast<char>('0' + v % 10);
		v /= 10;
	}
}

}  // namespace

std::string format_duration(Duration d) {
	int64_t ns = d.count();
	bool neg = ns < 0;
	uint64_t u = neg ? static_cast<uint64_t>(-(ns + 1)) + 1 : static_cast<uint64_t>(ns);
	std::string rev;
	if (u < 1000000000ULL) {
		rev += 's';
		int prec = 0;
		if (u == 0) {
			return "0s";
		} else if (u < 1000ULL) {
			rev += 'n';
		} else if (u < 1000000ULL) {
			prec = 3;
			rev += "\xb5\xc2";  // "µ" reversed byte order
		} else {
			prec = 6;
			rev += 'm';
		}
		u = fmt_frac(rev, u, prec);
		fmt_int(rev, u);
	} else {
		rev += 's';
		u = fmt_frac(rev, u, 9);
		fmt_int(rev, u % 60);
		u /= 60;
		if (u > 0) {
			rev += 'm';
			fmt_int(rev, u % 60);
			u /= 60;
			if (u > 0) {
				rev += 'h';
				fmt_int(rev, u);
			}
		}
	}
	if (neg) {
		rev += '-';
	}
	return std::string(rev.rbegin(), rev.rend());
}

Duration parse_duration(std::string_view orig) {
	auto fail = [&]() -> Error { return Error(std::format("invalid duration \"{}\"", orig)); };
	std::string_view s = orig;
	bool neg = false;
	if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
		neg = s[0] == '-';
		s.remove_prefix(1);
	}
	if (s == "0") {
		return Duration(0);
	}
	if (s.empty()) {
		throw fail();
	}
	constexpr uint64_t max = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1;
	uint64_t total = 0;
	while (!s.empty()) {
		if (!(s[0] == '.' || (s[0] >= '0' && s[0] <= '9'))) {
			throw fail();
		}
		uint64_t v = 0;
		size_t i = 0;
		bool pre = false;
		for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; i++) {
			if (v > (max - 9) / 10) {
				throw fail();
			}
			v = v * 10 + static_cast<uint64_t>(s[i] - '0');
			pre = true;
		}
		s.remove_prefix(i);
		uint64_t frac = 0;
		double scale = 1;
		bool post = false;
		if (!s.empty() && s[0] == '.') {
			s.remove_prefix(1);
			i = 0;
			for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; i++) {
				if (frac <= (max - 9) / 10) {
					frac = frac * 10 + static_cast<uint64_t>(s[i] - '0');
					scale *= 10;
				}
				post = true;
			}
			s.remove_prefix(i);
		}
		if (!pre && !post) {
			throw fail();
		}
		i = 0;
		while (i < s.size() && s[i] != '.' && !(s[i] >= '0' && s[i] <= '9')) {
			i++;
		}
		std::string_view unit = s.substr(0, i);
		s.remove_prefix(i);
		uint64_t mult;
		if (unit == "ns") {
			mult = 1;
		} else if (unit == "us" || unit == "\xc2\xb5s" || unit == "\xce\xbcs") {
			mult = 1000;
		} else if (unit == "ms") {
			mult = 1000000;
		} else if (unit == "s") {
			mult = 1000000000ULL;
		} else if (unit == "m") {
			mult = 60ULL * 1000000000ULL;
		} else if (unit == "h") {
			mult = 3600ULL * 1000000000ULL;
		} else {
			throw Error(std::format("invalid duration \"{}\": unknown unit \"{}\"", orig, unit));
		}
		if (v > max / mult) {
			throw fail();
		}
		v *= mult;
		if (frac > 0) {
			v += static_cast<uint64_t>(static_cast<double>(frac) * (static_cast<double>(mult) / scale));
			if (v > max) {
				throw fail();
			}
		}
		total += v;
		if (total > max) {
			throw fail();
		}
	}
	if (!neg && total > max - 1) {
		throw fail();
	}
	return Duration(neg ? -static_cast<int64_t>(total - 1) - 1 : static_cast<int64_t>(total));
}

}  // namespace etrace
