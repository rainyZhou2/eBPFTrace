#include "platform/auth.h"

#include <openssl/crypto.h>

#include <cctype>
#include <utility>

namespace etrace::auth {

namespace {

std::string_view trim(std::string_view s) {
	while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
		s.remove_prefix(1);
	}
	while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
		s.remove_suffix(1);
	}
	return s;
}

bool scheme_is(std::string_view value, std::string_view scheme) {
	if (value.size() < scheme.size() + 1) {
		return false;
	}
	for (size_t i = 0; i < scheme.size(); i++) {
		if (std::tolower(static_cast<unsigned char>(value[i])) !=
		    std::tolower(static_cast<unsigned char>(scheme[i]))) {
			return false;
		}
	}
	return value[scheme.size()] == ' ';
}

std::string_view payload(std::string_view value, std::string_view scheme) {
	value.remove_prefix(scheme.size() + 1);
	return trim(value);
}

int base64_value(char c) {
	if (c >= 'A' && c <= 'Z') {
		return c - 'A';
	}
	if (c >= 'a' && c <= 'z') {
		return c - 'a' + 26;
	}
	if (c >= '0' && c <= '9') {
		return c - '0' + 52;
	}
	if (c == '+') {
		return 62;
	}
	if (c == '/') {
		return 63;
	}
	return -1;
}

std::optional<std::string> base64_decode(std::string_view in) {
	std::string out;
	out.reserve(in.size() * 3 / 4);
	unsigned int acc = 0;
	int bits = 0;
	for (char c : in) {
		if (c == '=') {
			break;
		}
		if (std::isspace(static_cast<unsigned char>(c))) {
			continue;
		}
		int v = base64_value(c);
		if (v < 0) {
			return std::nullopt;
		}
		acc = (acc << 6) | static_cast<unsigned int>(v);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out.push_back(static_cast<char>((acc >> bits) & 0xff));
		}
	}
	return out;
}

}  // namespace

bool constant_time_equal(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) {
		return false;
	}
	if (a.empty()) {
		return true;
	}
	return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

TokenSet::TokenSet(std::vector<std::string> tokens) : tokens_(std::move(tokens)) {}

bool TokenSet::contains(std::string_view token) const {
	unsigned char found = 0;
	for (const auto& candidate : tokens_) {
		found |= static_cast<unsigned char>(constant_time_equal(token, candidate));
	}
	return found != 0;
}

std::optional<std::string> bearer_token(std::string_view header) {
	header = trim(header);
	if (!scheme_is(header, "Bearer")) {
		return std::nullopt;
	}
	auto token = payload(header, "Bearer");
	if (token.empty()) {
		return std::nullopt;
	}
	return std::string(token);
}

std::optional<BasicCredentials> basic_credentials(std::string_view header) {
	header = trim(header);
	if (!scheme_is(header, "Basic")) {
		return std::nullopt;
	}
	auto encoded = payload(header, "Basic");
	auto decoded = base64_decode(encoded);
	if (!decoded) {
		return std::nullopt;
	}
	auto colon = decoded->find(':');
	if (colon == std::string::npos) {
		return std::nullopt;
	}
	return BasicCredentials{decoded->substr(0, colon), decoded->substr(colon + 1)};
}

std::string basic_challenge(std::string_view realm) {
	return "Basic realm=\"" + std::string(realm) + "\"";
}

std::string bearer_challenge(std::string_view realm) {
	return "Bearer realm=\"" + std::string(realm) + "\"";
}

}  // namespace etrace::auth
