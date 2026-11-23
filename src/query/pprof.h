#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace etrace::query::pprof {

// Profiles use the pprof format (profile.proto, gzip compressed) understood
// by `go tool pprof`, Pyroscope and most profiling UIs. Frames are names
// only: eTrace symbolizes on the node, so locations carry one function line
// each and no addresses or mappings.

struct ValueType {
	std::string type;
	std::string unit;
	bool operator==(const ValueType&) const = default;
};

struct Sample {
	// stack is ordered from the innermost frame to the outermost.
	std::vector<std::string> stack;
	std::vector<int64_t> values;
	std::map<std::string, std::string> labels;
};

struct Profile {
	std::vector<ValueType> sample_types;
	ValueType period_type;
	int64_t period = 0;
	int64_t time_nanos = 0;
	int64_t duration_nanos = 0;
	std::vector<Sample> samples;

	// marshal returns the uncompressed profile.proto encoding.
	std::string marshal() const;
	// gzip returns the compressed form pprof tools expect.
	std::string gzip() const;
};

std::string gzip_compress(const std::string& data);

}  // namespace etrace::query::pprof
