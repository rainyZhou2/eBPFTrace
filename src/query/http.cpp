#include "query/http.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <sstream>
#include <utility>

#include "platform/compress.h"
#include "platform/hex.h"
#include "platform/http.h"

namespace etrace::query {

namespace {

class BadRequest : public Error {
public:
	using Error::Error;
};

int atoi_default(const std::string& v, int d) {
	int n;
	auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
	if (ec != std::errc() || p != v.data() + v.size() || n <= 0) {
		return d;
	}
	return n;
}

Time parse_time(const std::string& v, Time def) {
	if (v.empty()) {
		return def;
	}
	int64_t n;
	auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
	if (ec == std::errc() && p == v.data() + v.size()) {
		return from_unix(n);
	}
	if (auto t = parse_rfc3339(v)) {
		return *t;
	}
	throw BadRequest(std::format("invalid time \"{}\", want unix seconds or RFC 3339", v));
}

std::string trim(std::string s) {
	while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
		s.erase(s.begin());
	}
	while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
		s.pop_back();
	}
	return s;
}

std::string lower(std::string s) {
	for (auto& c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

constexpr size_t kMinGzipBytes = 1024;

void write_body(const httplib::Request& req, httplib::Response& res, int status, const std::string& body,
                const char* content_type) {
	res.status = status;
	res.set_header("Vary", "Accept-Encoding");
	if (body.size() >= kMinGzipBytes && compress::accepts_gzip(req.get_header_value("Accept-Encoding"))) {
		if (auto packed = compress::gzip(body)) {
			res.set_header("Content-Encoding", "gzip");
			res.set_content(*packed, content_type);
			return;
		}
	}
	res.set_content(body, content_type);
}

void write_json(const httplib::Request& req, httplib::Response& res, int status, const Json& j) {
	auto body = html_safe_dump(j);
	body += '\n';
	write_body(req, res, status, body, "application/json");
}

void write_error(const httplib::Request& req, httplib::Response& res, int status, const std::string& msg) {
	Json j;
	j["error"] = msg;
	write_json(req, res, status, j);
}

}  // namespace

Handler::Handler(Service& svc, tracing::Backend* backend, HandlerOptions opts, log::Logger log,
                 std::function<Time()> clock)
    : svc_(svc),
      backend_(backend),
      cfg_(opts.limits),
      auth_(std::move(opts.auth)),
      limiter_(std::move(opts.rate_limit)),
      trust_forwarded_for_(opts.trust_forwarded_for),
      log_(std::move(log)),
      now_(std::move(clock)) {}

std::string Handler::client_key(const httplib::Request& req) const {
	if (trust_forwarded_for_) {
		auto forwarded = req.get_header_value("X-Forwarded-For");
		if (auto comma = forwarded.find(','); comma != std::string::npos) {
			forwarded.resize(comma);
		}
		forwarded = trim(std::move(forwarded));
		if (!forwarded.empty()) {
			return forwarded;
		}
	}
	return req.remote_addr;
}

bool Handler::check_auth(const httplib::Request& req, httplib::Response& res) const {
	if (auth_.authorize(req.get_header_value("Authorization"))) {
		return true;
	}
	res.set_header("WWW-Authenticate", auth_.challenge());
	write_error(req, res, 401, "unauthorized");
	return false;
}

void Handler::routes(httplib::Server& srv) {
	srv.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
		if (!req.path.starts_with("/api/")) {
			return httplib::Server::HandlerResponse::Unhandled;
		}
		if (!limiter_.allow(client_key(req))) {
			write_error(req, res, 429, "rate limit exceeded");
			res.set_header("Retry-After", "1");
			return httplib::Server::HandlerResponse::Handled;
		}
		if (!check_auth(req, res)) {
			return httplib::Server::HandlerResponse::Handled;
		}
		return httplib::Server::HandlerResponse::Unhandled;
	});
	srv.Get("/api/v1/offcpu/flamegraph", [this](const auto& req, auto& res) { flamegraph(req, res); });
	srv.Get("/api/v1/offcpu/causes", [this](const auto& req, auto& res) { causes(req, res); });
	srv.Get("/api/v1/offcpu/events", [this](const auto& req, auto& res) { events(req, res); });
	srv.Get(R"(/api/v1/traces/([^/]+)/offcpu)",
	        [this](const auto& req, auto& res) { trace(req, res, req.matches[1].str()); });
}

int Handler::depth(const httplib::Request& req) const {
	auto v = req.get_param_value("depth");
	if (v == "0") {
		return 0;
	}
	return std::min(atoi_default(v, cfg_.max_chain_depth), cfg_.max_chain_depth);
}

int Handler::event_limit(const httplib::Request& req) const {
	return std::min(atoi_default(req.get_param_value("limit"), cfg_.max_event_result), cfg_.max_event_result);
}

storage::Filter Handler::parse_filter(const httplib::Request& req, Duration span) const {
	Time to, from;
	try {
		to = parse_time(req.get_param_value("to"), now_());
	} catch (const BadRequest& e) {
		throw BadRequest(std::format("to: {}", e.what()));
	}
	try {
		from = parse_time(req.get_param_value("from"), to - span);
	} catch (const BadRequest& e) {
		throw BadRequest(std::format("from: {}", e.what()));
	}
	if (!(from < to)) {
		throw BadRequest("from must precede to");
	}
	if (cfg_.max_range > Duration(0) && to - from > cfg_.max_range) {
		throw BadRequest(std::format("time range exceeds {}", format_duration(cfg_.max_range)));
	}
	storage::Filter f;
	f.from = from;
	f.to = to;
	f.node = req.get_param_value("node");
	f.comm = req.get_param_value("comm");
	f.container_id = req.get_param_value("container_id");
	f.ns = req.get_param_value("namespace");
	f.pod = req.get_param_value("pod");
	if (auto v = req.get_param_value("pid"); !v.empty()) {
		uint32_t pid;
		auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), pid);
		if (ec != std::errc() || p != v.data() + v.size()) {
			throw BadRequest(std::format("pid: invalid value \"{}\"", v));
		}
		f.pid = pid;
	}
	for (size_t i = 0, n = req.get_param_value_count("label"); i < n; i++) {
		auto l = req.get_param_value("label", i);
		auto eq = l.find('=');
		if (eq == std::string::npos || eq == 0) {
			throw BadRequest(std::format("label \"{}\" must be key=value", l));
		}
		f.labels[l.substr(0, eq)] = l.substr(eq + 1);
	}
	return f;
}

void Handler::internal(const httplib::Request& req, httplib::Response& res, const std::exception& e) const {
	log_.error("query failed", {{"err", e}});
	write_error(req, res, 500, "query failed");
}

void Handler::flamegraph(const httplib::Request& req, httplib::Response& res) {
	FlameRequest fr;
	try {
		fr = FlameRequest{parse_filter(req, cfg_.default_span), depth(req),
		                  atoi_default(req.get_param_value("limit"), 0)};
	} catch (const BadRequest& e) {
		return write_error(req, res, 400, e.what());
	}
	auto format = req.get_param_value("format");
	if (format != "" && format != "json" && format != "collapsed" && format != "pprof") {
		return write_error(req, res, 400, std::format("unknown format \"{}\"", format));
	}
	try {
		if (format == "collapsed") {
			std::ostringstream out;
			flame::write_collapsed(out, svc_.lines(fr));
			write_body(req, res, 200, out.str(), "text/plain; charset=utf-8");
		} else if (format == "pprof") {
			res.set_header("Content-Disposition", "attachment; filename=\"offcpu.pb.gz\"");
			res.set_content(svc_.profile(fr).gzip(), "application/octet-stream");
		} else {
			write_json(req, res, 200, flame::to_json(*svc_.tree(fr)));
		}
	} catch (const std::exception& e) {
		internal(req, res, e);
	}
}

void Handler::causes(const httplib::Request& req, httplib::Response& res) {
	storage::Filter f;
	try {
		f = parse_filter(req, cfg_.default_span);
	} catch (const BadRequest& e) {
		return write_error(req, res, 400, e.what());
	}
	try {
		Json j;
		j["causes"] = to_json(svc_.causes(f, depth(req), atoi_default(req.get_param_value("limit"), 50)));
		write_json(req, res, 200, j);
	} catch (const std::exception& e) {
		internal(req, res, e);
	}
}

void Handler::events(const httplib::Request& req, httplib::Response& res) {
	storage::EventQuery eq;
	try {
		eq.filter = parse_filter(req, cfg_.default_span);
		eq.trace_id = lower(req.get_param_value("trace_id"));
		eq.span_id = lower(req.get_param_value("span_id"));
		eq.limit = event_limit(req);
		if (!eq.trace_id.empty() && !is_lower_hex(eq.trace_id, 32)) {
			throw BadRequest("trace_id must be 32 hex characters");
		}
		if (!eq.span_id.empty() && !is_lower_hex(eq.span_id, 16)) {
			throw BadRequest("span_id must be 16 hex characters");
		}
		if (auto v = req.get_param_value("min_duration"); !v.empty()) {
			Duration d;
			try {
				d = parse_duration(v);
			} catch (const Error&) {
				throw BadRequest("min_duration must be a non-negative duration");
			}
			if (d < Duration(0)) {
				throw BadRequest("min_duration must be a non-negative duration");
			}
			eq.min_duration = d;
		}
	} catch (const BadRequest& e) {
		return write_error(req, res, 400, e.what());
	}
	try {
		Json j;
		j["events"] = to_json(svc_.reader().events(eq));
		write_json(req, res, 200, j);
	} catch (const std::exception& e) {
		internal(req, res, e);
	}
}

void Handler::trace(const httplib::Request& req, httplib::Response& res, const std::string& raw_id) {
	auto trace_id = lower(raw_id);
	storage::Filter f;
	try {
		if (!is_lower_hex(trace_id, 32)) {
			throw BadRequest("trace_id must be 32 hex characters");
		}
		f = parse_filter(req, cfg_.trace_lookback);
	} catch (const BadRequest& e) {
		return write_error(req, res, 400, e.what());
	}
	try {
		write_json(req, res, 200, to_json(svc_.trace_report(backend_, trace_id, f, event_limit(req))));
	} catch (const std::exception& e) {
		internal(req, res, e);
	}
}

}  // namespace etrace::query
