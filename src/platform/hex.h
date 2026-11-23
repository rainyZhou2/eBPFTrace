#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>

namespace etrace {

inline std::string hex_encode(std::span<const uint8_t> b) {
	static constexpr char digits[] = "0123456789abcdef";
	std::string out(b.size() * 2, '0');
	for (size_t i = 0; i < b.size(); i++) {
		out[2 * i] = digits[b[i] >> 4];
		out[2 * i + 1] = digits[b[i] & 0xf];
	}
	return out;
}

// hex_decode fills out from exactly out.size()*2 hex digits.
inline bool hex_decode(std::string_view s, std::span<uint8_t> out) {
	if (s.size() != out.size() * 2) {
		return false;
	}
	auto nibble = [](char c) -> int {
		if (c >= '0' && c <= '9')
			return c - '0';
		if (c >= 'a' && c <= 'f')
			return c - 'a' + 10;
		if (c >= 'A' && c <= 'F')
			return c - 'A' + 10;
		return -1;
	};
	for (size_t i = 0; i < out.size(); i++) {
		int hi = nibble(s[2 * i]), lo = nibble(s[2 * i + 1]);
		if (hi < 0 || lo < 0) {
			return false;
		}
		out[i] = static_cast<uint8_t>(hi << 4 | lo);
	}
	return true;
}

// random_hex returns n random bytes from the system CSPRNG as hex.
inline std::string random_hex(size_t n) {
	std::random_device rd;
	std::string out;
	std::array<uint8_t, 64> buf{};
	n = std::min(n, buf.size());
	for (size_t i = 0; i < n; i += 4) {
		uint32_t v = rd();
		for (size_t j = 0; j < 4 && i + j < n; j++) {
			buf[i + j] = static_cast<uint8_t>(v >> (8 * j));
		}
	}
	return hex_encode(std::span<const uint8_t>(buf.data(), n));
}

inline bool is_lower_hex(std::string_view s, size_t len) {
	if (s.size() != len) {
		return false;
	}
	for (char c : s) {
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
			return false;
		}
	}
	return true;
}

}  // namespace etrace
