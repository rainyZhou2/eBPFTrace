#include "platform/compress.h"

#include <zlib.h>

#include <cctype>
#include <charconv>
#include <limits>

namespace etrace::compress {

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

std::string lower(std::string_view s) {
	std::string out;
	out.reserve(s.size());
	for (char c : s) {
		out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return out;
}

double quality(std::string_view parameter) {
	parameter = trim(parameter);
	if (!parameter.starts_with("q=")) {
		return 1.0;
	}
	double value = 0;
	auto text = parameter.substr(2);
	auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
	if (ec != std::errc() || ptr != text.data() + text.size()) {
		return 0;
	}
	return value;
}

}  // namespace

bool accepts_gzip(std::string_view accept_encoding) {
	double gzip_quality = -1;
	double wildcard_quality = -1;
	while (!accept_encoding.empty()) {
		auto comma = accept_encoding.find(',');
		auto entry = trim(accept_encoding.substr(0, comma));
		if (!entry.empty()) {
			auto semicolon = entry.find(';');
			auto coding = lower(trim(entry.substr(0, semicolon)));
			double q = 1.0;
			if (semicolon != std::string_view::npos) {
				q = quality(entry.substr(semicolon + 1));
			}
			if (coding == "gzip") {
				gzip_quality = q;
			} else if (coding == "*") {
				wildcard_quality = q;
			}
		}
		if (comma == std::string_view::npos) {
			break;
		}
		accept_encoding.remove_prefix(comma + 1);
	}
	return gzip_quality > 0 || (gzip_quality < 0 && wildcard_quality > 0);
}

std::optional<std::string> gzip(std::string_view data) {
	if (data.size() > std::numeric_limits<uInt>::max()) {
		return std::nullopt;
	}
	z_stream zs{};
	if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
		return std::nullopt;
	}
	auto bound = deflateBound(&zs, static_cast<uLong>(data.size()));
	if (bound > std::numeric_limits<uInt>::max() - 32) {
		deflateEnd(&zs);
		return std::nullopt;
	}
	std::string out;
	out.resize(static_cast<size_t>(bound) + 32);
	zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
	zs.avail_in = static_cast<uInt>(data.size());
	zs.next_out = reinterpret_cast<Bytef*>(out.data());
	zs.avail_out = static_cast<uInt>(out.size());
	int rc = deflate(&zs, Z_FINISH);
	deflateEnd(&zs);
	if (rc != Z_STREAM_END) {
		return std::nullopt;
	}
	out.resize(zs.total_out);
	return out;
}

}  // namespace etrace::compress
