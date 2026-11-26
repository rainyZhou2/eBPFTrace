#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace etrace::compress {

// accepts_gzip reports whether the Accept-Encoding header allows gzip.
bool accepts_gzip(std::string_view accept_encoding);

// gzip returns the gzip-compressed input, or nothing when zlib cannot
// compress it.
std::optional<std::string> gzip(std::string_view data);

}  // namespace etrace::compress
