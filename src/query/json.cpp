#include "query/json.h"

#include <format>

namespace etrace::query {

bool has_overlapping_wait(const storage::Hop& h) {
	return h.wait_ns != 0 || !h.wait_stack.empty();
}

Json to_json(const storage::Hop& h) {
	Json j;
	j["comm"] = h.comm;
	if (h.pid != 0) {
		j["pid"] = h.pid;
	}
	if (!h.wake_stack.empty()) {
		j["wake_stack"] = h.wake_stack;
	}
	if (!h.wait_stack.empty()) {
		j["wait_stack"] = h.wait_stack;
	}
	if (h.wait_ns != 0) {
		j["wait_ns"] = h.wait_ns;
	}
	if (h.wake_evidence != model::Evidence::unknown) {
		j["wake_evidence"] = model::to_string(h.wake_evidence);
	}
	if (h.wait_evidence != model::Evidence::unknown) {
		j["wait_evidence"] = model::to_string(h.wait_evidence);
	} else if (has_overlapping_wait(h)) {
		j["wait_evidence"] = "inferred";
	}
	if (!h.trace_id.empty()) {
		j["trace_id"] = h.trace_id;
	}
	if (!h.span_id.empty()) {
		j["span_id"] = h.span_id;
	}
	return j;
}

Json to_json(const storage::Event& e) {
	Json j;
	j["node"] = e.node;
	j["pid"] = e.pid;
	j["tid"] = e.tid;
	j["comm"] = e.comm;
	if (!e.container_id.empty()) {
		j["container_id"] = e.container_id;
	}
	if (!e.ns.empty()) {
		j["namespace"] = e.ns;
	}
	if (!e.pod.empty()) {
		j["pod"] = e.pod;
	}
	if (!e.container.empty()) {
		j["container"] = e.container;
	}
	j["stack"] = e.stack;
	if (!e.chain.empty()) {
		auto& c = j["chain"] = Json::array();
		for (const auto& h : e.chain) {
			auto hop = to_json(h);
			if (!hop.contains("wake_evidence")) {
				hop["wake_evidence"] = c.empty() ? "observed" : "inferred";
			}
			c.push_back(std::move(hop));
		}
	}
	j["start"] = format_rfc3339nano(e.start);
	j["end"] = format_rfc3339nano(e.end);
	j["reason"] = e.reason;
	if (e.reason == "gpu") {
		j["gpu_stream"] = std::format("{:#x}", e.gpu_stream);
		j["gpu_launches"] = e.gpu_launches;
	}
	if (!e.trace_id.empty()) {
		j["trace_id"] = e.trace_id;
	}
	if (!e.span_id.empty()) {
		j["span_id"] = e.span_id;
	}
	return j;
}

Json to_json(const std::vector<storage::Event>& events) {
	auto arr = Json::array();
	for (const auto& e : events) {
		arr.push_back(to_json(e));
	}
	return arr;
}

std::string html_safe_dump(const Json& j) {
	auto s = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); i++) {
		char c = s[i];
		if (c == '<') {
			out += "\\u003c";
		} else if (c == '>') {
			out += "\\u003e";
		} else if (c == '&') {
			out += "\\u0026";
		} else if (c == '\xe2' && i + 2 < s.size() && s[i + 1] == '\x80' &&
		           (s[i + 2] == '\xa8' || s[i + 2] == '\xa9')) {
			out += s[i + 2] == '\xa8' ? "\\u2028" : "\\u2029";
			i += 2;
		} else {
			out += c;
		}
	}
	return out;
}

}  // namespace etrace::query
