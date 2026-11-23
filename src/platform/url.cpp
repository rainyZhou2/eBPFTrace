#include "platform/url.h"

#include <cctype>
#include <cstdint>
#include <format>

namespace etrace {

std::optional<HttpUrl> parse_http_url(std::string_view raw) {
	auto sep = raw.find("://");
	if (sep == std::string_view::npos) {
		return std::nullopt;
	}
	HttpUrl u;
	for (char c : raw.substr(0, sep)) {
		u.scheme += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	if (u.scheme != "http" && u.scheme != "https") {
		return std::nullopt;
	}
	auto rest = raw.substr(sep + 3);
	auto end = rest.find_first_of("/?#");
	u.host = std::string(rest.substr(0, end));
	if (u.host.empty() || u.host.find('@') != std::string::npos) {
		return std::nullopt;
	}
	for (char c : u.host) {
		if (std::isspace(static_cast<unsigned char>(c))) {
			return std::nullopt;
		}
	}
	if (end != std::string_view::npos && rest[end] == '/') {
		auto path = rest.substr(end);
		path = path.substr(0, path.find_first_of("?#"));
		while (!path.empty() && path.back() == '/') {
			path.remove_suffix(1);
		}
		u.path = std::string(path);
	}
	return u;
}

std::string path_escape(std::string_view s) {
	std::string out;
	for (unsigned char c : s) {
		if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			out += static_cast<char>(c);
		} else {
			out += std::format("%{:02X}", c);
		}
	}
	return out;
}

std::string base64_encode(std::string_view in) {
	static constexpr char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve((in.size() + 2) / 3 * 4);
	size_t i = 0;
	for (; i + 2 < in.size(); i += 3) {
		uint32_t v = static_cast<uint8_t>(in[i]) << 16 | static_cast<uint8_t>(in[i + 1]) << 8 |
		             static_cast<uint8_t>(in[i + 2]);
		out += tbl[v >> 18];
		out += tbl[(v >> 12) & 63];
		out += tbl[(v >> 6) & 63];
		out += tbl[v & 63];
	}
	if (i < in.size()) {
		uint32_t v = static_cast<uint8_t>(in[i]) << 16;
		if (i + 1 < in.size()) {
			v |= static_cast<uint8_t>(in[i + 1]) << 8;
		}
		out += tbl[v >> 18];
		out += tbl[(v >> 12) & 63];
		out += i + 1 < in.size() ? tbl[(v >> 6) & 63] : '=';
		out += '=';
	}
	return out;
}

}  // namespace etrace
