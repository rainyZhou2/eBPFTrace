#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace etrace {

// HttpUrl is an absolute http(s) URL split into the origin a client
// connects to and the path prefix requests are built on.
struct HttpUrl {
	std::string scheme;
	std::string host;  // host or host:port, brackets kept for IPv6
	std::string path;  // without trailing slash

	std::string origin() const { return scheme + "://" + host; }
};

// parse_http_url accepts only absolute http and https URLs with a host.
std::optional<HttpUrl> parse_http_url(std::string_view raw);

// path_escape percent-encodes s for use as one URL path segment.
std::string path_escape(std::string_view s);

std::string base64_encode(std::string_view in);

}  // namespace etrace
