#include "tracing/jaeger.h"

#include <httplib.h>

#include <format>
#include <nlohmann/json.hpp>

namespace etrace::tracing::jaeger {

namespace {

constexpr size_t kMaxBody = 64 << 20;

std::string normalize_id(std::string id, size_t width) {
	for (auto& c : id) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	if (id.size() < width) {
		id.insert(0, width - id.size(), '0');
	}
	return id;
}

std::string tag_value(const nlohmann::json& v) {
	if (v.is_string()) {
		return v.get<std::string>();
	}
	if (v.is_number_float()) {
		return std::format("{}", v.get<double>());
	}
	if (v.is_null()) {
		return "<nil>";
	}
	return v.dump();
}

std::vector<Span> convert(const nlohmann::json& data) {
	std::vector<Span> out;
	for (const auto& t : data) {
		std::map<std::string, std::string> services;
		if (auto it = t.find("processes"); it != t.end() && it->is_object()) {
			for (const auto& [id, p] : it->items()) {
				services[id] = p.value("serviceName", "");
			}
		}
		for (const auto& s : t.value("spans", nlohmann::json::array())) {
			Span sp;
			sp.span_id = normalize_id(s.value("spanID", ""), 16);
			sp.name = s.value("operationName", "");
			sp.service = services[s.value("processID", "")];
			int64_t start = s.value("startTime", int64_t{0});
			int64_t dur = s.value("duration", int64_t{0});
			sp.start = from_unix_micro(start);
			sp.end = from_unix_micro(start + dur);
			for (const auto& ref : s.value("references", nlohmann::json::array())) {
				if (ref.value("refType", "") == "CHILD_OF") {
					sp.parent_id = normalize_id(ref.value("spanID", ""), 16);
					break;
				}
			}
			for (const auto& tag : s.value("tags", nlohmann::json::array())) {
				sp.attrs[tag.value("key", "")] =
				        tag_value(tag.contains("value") ? tag["value"] : nlohmann::json());
			}
			out.push_back(std::move(sp));
		}
	}
	return out;
}

}  // namespace

void Options::validate() const {
	if (!parse_http_url(url)) {
		throw Error("url must be an http(s) URL");
	}
}

void decode_fields(config::Fields& f, Options& o) {
	f("url", o.url)("timeout", o.timeout)("token", o.token);
}

Backend::Backend(Options o) : opts_(std::move(o)), base_(*parse_http_url(opts_.url)) {}

std::vector<Span> Backend::trace(const std::string& trace_id) {
	httplib::Client cli(base_.origin());
	auto secs = std::chrono::duration_cast<std::chrono::microseconds>(opts_.timeout);
	cli.set_connection_timeout(secs);
	cli.set_read_timeout(secs);
	cli.set_write_timeout(secs);
	httplib::Headers headers;
	if (!opts_.token.empty()) {
		headers.emplace("Authorization", "Bearer " + opts_.token);
	}
	std::string body;
	bool too_large = false;
	auto res =
	        cli.Get(base_.path + "/api/traces/" + path_escape(trace_id), headers, [&](const char* data, size_t n) {
		        if (body.size() + n > kMaxBody) {
			        too_large = true;
			        return false;
		        }
		        body.append(data, n);
		        return true;
	        });
	if (too_large) {
		throw Error("jaeger: response exceeds 64MiB");
	}
	if (!res) {
		throw Error(std::format("jaeger: {}", httplib::to_string(res.error())));
	}
	if (res->status == 404) {
		throw NotFound();
	}
	if (res->status != 200) {
		auto msg = body.substr(0, 512);
		while (!msg.empty() && std::isspace(static_cast<unsigned char>(msg.back()))) {
			msg.pop_back();
		}
		throw Error(std::format("jaeger: status {}: {}", res->status, msg));
	}
	nlohmann::json data;
	try {
		data = nlohmann::json::parse(body).value("data", nlohmann::json::array());
		if (data.empty()) {
			throw NotFound();
		}
		return convert(data);
	} catch (const nlohmann::json::exception& e) {
		throw Error(std::format("jaeger: decode: {}", e.what()));
	}
}

}  // namespace etrace::tracing::jaeger
