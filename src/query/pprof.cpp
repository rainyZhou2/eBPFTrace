#include "query/pprof.h"

#include <unordered_map>
#include <utility>

#include "platform/compress.h"
#include "platform/error.h"

namespace etrace::query::pprof {

namespace {

// profile.proto field numbers.
enum : uint32_t {
	kProfileSampleType = 1,
	kProfileSample = 2,
	kProfileLocation = 4,
	kProfileFunction = 5,
	kProfileStringTable = 6,
	kProfileTimeNanos = 9,
	kProfileDuration = 10,
	kProfilePeriodType = 11,
	kProfilePeriod = 12,

	kValueTypeType = 1,
	kValueTypeUnit = 2,

	kSampleLocationID = 1,
	kSampleValue = 2,
	kSampleLabel = 3,

	kLabelKey = 1,
	kLabelStr = 2,

	kLocationID = 1,
	kLocationLine = 4,

	kLineFunctionID = 1,

	kFunctionID = 1,
	kFunctionName = 2,
	kFunctionSystemName = 3,
};

constexpr uint32_t kVarint = 0;
constexpr uint32_t kBytes = 2;

void put_varint(std::string& b, uint64_t v) {
	while (v >= 0x80) {
		b += static_cast<char>(v | 0x80);
		v >>= 7;
	}
	b += static_cast<char>(v);
}

void put_tag(std::string& b, uint32_t field, uint32_t wire) {
	put_varint(b, uint64_t{field} << 3 | wire);
}

void put_message(std::string& b, uint32_t field, const std::string& msg) {
	put_tag(b, field, kBytes);
	put_varint(b, msg.size());
	b += msg;
}

void put_uint(std::string& b, uint32_t field, uint64_t v) {
	if (v == 0) {
		return;
	}
	put_tag(b, field, kVarint);
	put_varint(b, v);
}

void put_packed(std::string& b, uint32_t field, const std::vector<uint64_t>& vs) {
	if (vs.empty()) {
		return;
	}
	std::string packed;
	for (auto v : vs) {
		put_varint(packed, v);
	}
	put_message(b, field, packed);
}

struct Encoder {
	std::unordered_map<std::string, int64_t> strings{{"", 0}};
	std::vector<std::string> table{""};
	std::unordered_map<std::string, uint64_t> funcs;
	std::vector<std::string> order;

	int64_t str(const std::string& s) {
		auto [it, inserted] = strings.try_emplace(s, static_cast<int64_t>(table.size()));
		if (inserted) {
			table.push_back(s);
		}
		return it->second;
	}

	// location returns the ID shared by the location and function of a
	// frame name; one location per function is exact without addresses.
	uint64_t location(const std::string& name) {
		auto [it, inserted] = funcs.try_emplace(name, order.size() + 1);
		if (inserted) {
			order.push_back(name);
			str(name);
		}
		return it->second;
	}

	std::string value_type(const ValueType& vt) {
		std::string b;
		put_uint(b, kValueTypeType, static_cast<uint64_t>(str(vt.type)));
		put_uint(b, kValueTypeUnit, static_cast<uint64_t>(str(vt.unit)));
		return b;
	}
};

}  // namespace

std::string Profile::marshal() const {
	Encoder e;
	std::string out;
	for (const auto& st : sample_types) {
		put_message(out, kProfileSampleType, e.value_type(st));
	}
	for (const auto& s : samples) {
		std::string msg;
		std::vector<uint64_t> locs;
		locs.reserve(s.stack.size());
		for (const auto& f : s.stack) {
			locs.push_back(e.location(f));
		}
		put_packed(msg, kSampleLocationID, locs);
		std::vector<uint64_t> vals;
		vals.reserve(s.values.size());
		for (auto v : s.values) {
			vals.push_back(static_cast<uint64_t>(v));
		}
		put_packed(msg, kSampleValue, vals);
		for (const auto& [k, v] : s.labels) {
			std::string l;
			put_uint(l, kLabelKey, static_cast<uint64_t>(e.str(k)));
			put_uint(l, kLabelStr, static_cast<uint64_t>(e.str(v)));
			put_message(msg, kSampleLabel, l);
		}
		put_message(out, kProfileSample, msg);
	}
	for (size_t i = 0; i < e.order.size(); i++) {
		uint64_t id = i + 1;
		std::string line, loc, fn;
		put_uint(line, kLineFunctionID, id);
		put_uint(loc, kLocationID, id);
		put_message(loc, kLocationLine, line);
		put_message(out, kProfileLocation, loc);
		auto name_idx = static_cast<uint64_t>(e.str(e.order[i]));
		put_uint(fn, kFunctionID, id);
		put_uint(fn, kFunctionName, name_idx);
		put_uint(fn, kFunctionSystemName, name_idx);
		put_message(out, kProfileFunction, fn);
	}
	auto pt = e.value_type(period_type);
	for (const auto& s : e.table) {
		put_message(out, kProfileStringTable, s);
	}
	put_uint(out, kProfileTimeNanos, static_cast<uint64_t>(time_nanos));
	put_uint(out, kProfileDuration, static_cast<uint64_t>(duration_nanos));
	if (!(period_type == ValueType{})) {
		put_message(out, kProfilePeriodType, pt);
	}
	put_uint(out, kProfilePeriod, static_cast<uint64_t>(period));
	return out;
}

std::string Profile::gzip() const {
	auto out = compress::gzip(marshal());
	if (!out) {
		throw Error("gzip: compress failed");
	}
	return std::move(*out);
}

}  // namespace etrace::query::pprof
