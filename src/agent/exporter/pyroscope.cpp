#include "agent/exporter/pyroscope.h"

#include <httplib.h>

#include <algorithm>
#include <format>
#include <map>
#include <nlohmann/json.hpp>

#include "platform/hex.h"
#include "query/service.h"
#include "storage/storage.h"

namespace etrace::agent::pyroscope {

namespace {

std::string sample_id() {
	auto h = random_hex(16);
	return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" +
	       h.substr(20);
}

}  // namespace

void Options::validate() const {
	if (!parse_http_url(url)) {
		throw Error(std::format("url \"{}\" must be an absolute http(s) URL", url));
	}
	if (service_name.empty()) {
		throw Error("service_name is required");
	}
	if (timeout <= Duration(0)) {
		throw Error("timeout must be positive");
	}
}

void decode_fields(config::Fields& f, Options& o) {
	f("url", o.url)("service_name", o.service_name)("tenant_id", o.tenant_id)("username", o.username)(
	        "password", o.password)("timeout", o.timeout);
}

Exporter::Exporter(Options o) : opts_(std::move(o)), base_(*parse_http_url(opts_.url)) {}

std::vector<std::pair<std::string, std::string>> Exporter::labels(const model::Resource& r, const model::Task& t,
                                                                  bool gpu) const {
	std::vector<std::pair<std::string, std::string>> ls{{"__name__", gpu ? "gpu" : "offcpu"},
	                                                    {"service_name", opts_.service_name},
	                                                    {"node", r.node},
	                                                    {"comm", t.comm}};
	for (const auto& [k, v] : std::map<std::string, std::string>{
	             {"namespace", t.pod.ns}, {"pod", t.pod.name}, {"container", t.pod.container}}) {
		if (!v.empty()) {
			ls.emplace_back(k, v);
		}
	}
	for (const auto& [k, v] : r.labels) {
		ls.emplace_back(k, v);
	}
	std::stable_sort(ls.begin(), ls.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	return ls;
}

// build_body groups samples into one series per task identity and profile
// type: GPU waits are sent as gpu:gpu_wait:nanoseconds so they do not add
// to the off-CPU profile.
std::string Exporter::build_body(const model::Batch& b) const {
	std::map<std::string, std::vector<storage::ChainValue>> groups;
	std::map<std::string, model::Task> tasks;
	for (const auto& s : b.offcpu) {
		std::string key = std::string(s.reason == model::OffCPUReason::gpu ? "g" : "o") + '\0' + s.task.comm +
		                  '\0' + s.task.pod.ns + '\0' + s.task.pod.name + '\0' + s.task.pod.container;
		tasks[key] = s.task;
		groups[key].push_back(storage::ChainValue{s.task.comm, model::symbols(s.stack), storage::hops(s.chain),
		                                          s.blocked_ns, s.count});
	}
	if (groups.empty()) {
		return "";
	}
	nlohmann::ordered_json series = nlohmann::ordered_json::array();
	for (const auto& [key, vals] : groups) {
		bool gpu = key[0] == 'g';
		auto raw =
		        query::offcpu_profile(vals, b.window_start, b.window_end, gpu ? "gpu_wait" : "off_cpu").gzip();
		nlohmann::ordered_json ls = nlohmann::ordered_json::array();
		for (const auto& [n, v] : labels(b.resource, tasks[key], gpu)) {
			ls.push_back({{"name", n}, {"value", v}});
		}
		nlohmann::ordered_json sample;
		sample["ID"] = sample_id();
		sample["rawProfile"] = base64_encode(raw);
		nlohmann::ordered_json s;
		s["labels"] = std::move(ls);
		s["samples"] = nlohmann::ordered_json::array({std::move(sample)});
		series.push_back(std::move(s));
	}
	nlohmann::ordered_json body;
	body["series"] = std::move(series);
	return body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

void Exporter::export_batch(std::stop_token, const model::Batch& b) {
	std::string body;
	try {
		body = build_body(b);
	} catch (const std::exception& e) {
		throw PermanentError(e.what());
	}
	if (body.empty()) {
		return;
	}
	httplib::Client cli(base_.origin());
	auto t = std::chrono::duration_cast<std::chrono::microseconds>(opts_.timeout);
	cli.set_connection_timeout(t);
	cli.set_read_timeout(t);
	cli.set_write_timeout(t);
	httplib::Headers headers;
	if (!opts_.tenant_id.empty()) {
		headers.emplace("X-Scope-OrgID", opts_.tenant_id);
	}
	if (!opts_.username.empty()) {
		cli.set_basic_auth(opts_.username, opts_.password);
	}
	auto res = cli.Post(base_.path + "/push.v1.PusherService/Push", headers, body, "application/json");
	if (!res) {
		throw Error(std::format("push to pyroscope: {}", httplib::to_string(res.error())));
	}
	auto msg = res->body.substr(0, 4096);
	if (res->status < 300) {
		return;
	}
	auto err = std::format("pyroscope: {} {}: {}", res->status, httplib::status_message(res->status), msg);
	if (res->status == 429 || res->status >= 500) {
		throw Error(err);
	}
	throw PermanentError(err);
}

}  // namespace etrace::agent::pyroscope
