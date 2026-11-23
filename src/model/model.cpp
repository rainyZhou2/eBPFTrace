#include "model/model.h"

#include "platform/hex.h"

namespace etrace::model {

std::string Frame::symbol() const {
	std::string name = function.empty() ? "[unknown]" : function;
	if (module.empty()) {
		return name;
	}
	return module + "`" + name;
}

std::vector<std::string> symbols(const Stack& s) {
	std::vector<std::string> out;
	out.reserve(s.size());
	for (const auto& f : s) {
		out.push_back(f.symbol());
	}
	return out;
}

const char* to_string(OffCPUReason r) {
	switch (r) {
		case OffCPUReason::blocked:
			return "blocked";
		case OffCPUReason::preempted:
			return "preempted";
		case OffCPUReason::gpu:
			return "gpu";
		default:
			return "unknown";
	}
}

std::string TraceContext::trace_id_hex() const {
	return valid() ? hex_encode(trace_id) : "";
}

std::string TraceContext::span_id_hex() const {
	return valid() ? hex_encode(span_id) : "";
}

std::optional<TraceContext> parse_trace_context(std::string_view trace_id, std::string_view span_id) {
	TraceContext tc;
	if (!hex_decode(trace_id, tc.trace_id)) {
		return std::nullopt;
	}
	if (!span_id.empty() && !hex_decode(span_id, tc.span_id)) {
		return std::nullopt;
	}
	if (!tc.valid()) {
		return std::nullopt;
	}
	return tc;
}

}  // namespace etrace::model
