#include <gtest/gtest.h>
#include <httplib.h>
#include <zlib.h>

#include <atomic>
#include <cstdlib>
#include <format>
#include <nlohmann/json.hpp>
#include <thread>

#include "agent/exporter/grpc.h"
#include "agent/exporter/pyroscope.h"
#include "collector/auth.h"
#include "collector/grpc.h"
#include "platform/grpc.h"
#include "platform/ratelimit.h"
#include "query/auth.h"
#include "query/http.h"
#include "query/service.h"
#include "query/ui.h"
#include "storage/memory/memory.h"
#include "symbol/debuginfod.h"
#include "testutil.h"
#include "tracing/jaeger.h"
#include "wire/wire.h"

namespace etrace {
namespace {

// TestServer configures an httplib::Server and serves it on a loopback
// port for one test.
class TestServer {
public:
	explicit TestServer(const std::function<void(httplib::Server&)>& setup) {
		setup(srv_);
		port_ = srv_.bind_to_any_port("127.0.0.1");
		thread_ = std::thread([this] { srv_.listen_after_bind(); });
		srv_.wait_until_ready();
	}
	~TestServer() {
		srv_.stop();
		thread_.join();
	}
	std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }

private:
	httplib::Server srv_;
	int port_;
	std::thread thread_;
};

TEST(Jaeger, Trace) {
	TestServer r([](httplib::Server& s) {
		s.Get(R"(/api/traces/(.*))", [](const httplib::Request& req, httplib::Response& res) {
			if (req.matches[1] != "4bf92f3577b34da6a3ce929d0e0e4736") {
				res.status = 404;
				return;
			}
			res.set_content(
			        R"({"data":[{"traceID":"4bf92f3577b34da6a3ce929d0e0e4736","processes":{"p1":{"serviceName":"api"}},
			"spans":[{"spanID":"a2fb4a1d1a96d312","operationName":"GET /","startTime":1000,"duration":500,"processID":"p1","references":[]},
			{"spanID":"f067aa0ba902b7","operationName":"db","startTime":1100,"duration":200,"processID":"p1",
			 "references":[{"refType":"CHILD_OF","spanID":"a2fb4a1d1a96d312"}],"tags":[{"key":"db.system","value":"pg"},{"key":"n","value":3}]}]}]})",
			        "application/json");
		});
	});
	tracing::jaeger::Backend b(tracing::jaeger::Options{r.url(), 5s, ""});
	auto spans = b.trace("4bf92f3577b34da6a3ce929d0e0e4736");
	ASSERT_EQ(spans.size(), 2u);
	EXPECT_EQ(spans[1].parent_id, "a2fb4a1d1a96d312");
	EXPECT_EQ(spans[1].span_id, "00f067aa0ba902b7");
	EXPECT_EQ(spans[0].service, "api");
	EXPECT_EQ(spans[1].attrs["db.system"], "pg");
	EXPECT_EQ(spans[1].attrs["n"], "3");
	EXPECT_EQ(spans[1].duration(), 200us);
	EXPECT_THROW(b.trace("ffffffffffffffffffffffffffffffff"), tracing::NotFound);
}

TEST(UI, ServesPageAndAssetsWithPolicy) {
	TestServer r([](httplib::Server& s) { query::ui::routes(s); });
	httplib::Client c(r.url());
	for (auto [path, want] : std::vector<std::pair<std::string, std::string>>{
	             {"/", "<title>eTrace</title>"}, {"/ui/app.js", "textContent"}, {"/ui/app.css", "#flame-graph"}}) {
		auto res = c.Get(path);
		ASSERT_TRUE(res) << path;
		EXPECT_EQ(res->status, 200) << path;
		EXPECT_NE(res->body.find(want), std::string::npos) << path;
		EXPECT_NE(res->get_header_value("Content-Security-Policy").find("script-src 'self'"),
		          std::string::npos);
	}
	auto res = c.Get("/ui/../../CMakeLists.txt");
	EXPECT_TRUE(!res || res->status != 200 || res->body.find("project(") == std::string::npos);
}

TEST(QueryHTTP, ValidatesParametersAndServesFormats) {
	storage::memory::Store store(storage::memory::Options{Duration(0)});
	model::Batch b;
	b.id = "1";
	b.resource.node = "n1";
	b.window_start = etrace::now() - 1s;
	b.window_end = etrace::now();
	b.offcpu.push_back(
	        model::OffCPUSample{model::Task{1, 1, "api", "", {}}, testing::frames({"futex_wait"}), {}, 10, 1, {}});
	store.write_batch(b);
	query::Service svc(store);
	query::Handler h(svc, nullptr,
	                 query::HandlerOptions{query::HandlerConfig{7 * 24h, 15min, 24h, 4, 100},
	                                       query::AuthConfig{.allow_unauthenticated = true},
	                                       ratelimit::Policy{.enabled = false}, false},
	                 log::Logger::discard());
	TestServer r([&](httplib::Server& s) { h.routes(s); });
	httplib::Client c(r.url());

	auto res = c.Get("/api/v1/offcpu/flamegraph?format=collapsed");
	ASSERT_TRUE(res);
	EXPECT_EQ(res->status, 200);
	EXPECT_EQ(res->body, "api;futex_wait 10\n");
	res = c.Get("/api/v1/offcpu/flamegraph");
	EXPECT_EQ(nlohmann::json::parse(res->body)["value"], 10);
	res = c.Get("/api/v1/offcpu/flamegraph?format=pprof");
	EXPECT_EQ(res->get_header_value("Content-Type"), "application/octet-stream");
	res = c.Get("/api/v1/offcpu/flamegraph?format=svg");
	EXPECT_EQ(res->status, 400);
	res = c.Get("/api/v1/offcpu/causes?from=10&to=5");
	EXPECT_EQ(res->status, 400);
	EXPECT_EQ(nlohmann::json::parse(res->body)["error"], "from must precede to");
	res = c.Get("/api/v1/offcpu/events?trace_id=xyz");
	EXPECT_EQ(res->status, 400);
	res = c.Get("/api/v1/offcpu/events?min_duration=-1s");
	EXPECT_EQ(res->status, 400);
	res = c.Get("/api/v1/offcpu/events?label=nokey");
	EXPECT_EQ(res->status, 400);
	res = c.Get("/api/v1/offcpu/causes?from=2020-01-01T00:00:00Z&to=2024-01-01T00:00:00Z");
	EXPECT_EQ(res->status, 400) << "range above max_range";
	res = c.Get("/api/v1/traces/4BF92F3577B34DA6A3CE929D0E0E4736/offcpu");
	ASSERT_EQ(res->status, 200);
	auto j = nlohmann::json::parse(res->body);
	EXPECT_EQ(j["trace_id"], "4bf92f3577b34da6a3ce929d0e0e4736");
	EXPECT_EQ(j["partial"], "no trace backend configured");
	res = c.Get("/api/v1/traces/abc/offcpu");
	EXPECT_EQ(res->status, 400);
}

TEST(QueryHTTP, RequiresCredentialsAndKeepsUIPublic) {
	storage::memory::Store store(storage::memory::Options{Duration(0)});
	query::Service svc(store);
	query::AuthConfig auth;
	auth.tokens = {"query-secret"};
	auth.basic_user = "ops";
	auth.basic_password = "pw";
	query::Handler h(svc, nullptr,
	                 query::HandlerOptions{query::HandlerConfig{7 * 24h, 15min, 24h, 4, 100}, auth,
	                                       ratelimit::Policy{.enabled = false}, false},
	                 log::Logger::discard());
	TestServer r([&](httplib::Server& s) {
		h.routes(s);
		query::ui::routes(s);
	});
	httplib::Client c(r.url());

	auto res = c.Get("/api/v1/offcpu/flamegraph");
	ASSERT_TRUE(res);
	EXPECT_EQ(res->status, 401);
	EXPECT_NE(res->get_header_value("WWW-Authenticate").find("Basic"), std::string::npos);

	res = c.Get("/");
	ASSERT_TRUE(res);
	EXPECT_EQ(res->status, 200);

	res = c.Get("/api/v1/offcpu/flamegraph", httplib::Headers{{"Authorization", "Bearer query-secret"}});
	ASSERT_TRUE(res);
	EXPECT_EQ(res->status, 200);

	res = c.Get("/api/v1/offcpu/flamegraph", httplib::Headers{{"Authorization", "Bearer wrong"}});
	ASSERT_TRUE(res);
	EXPECT_EQ(res->status, 401);

	c.set_basic_auth("ops", "pw");
	res = c.Get("/api/v1/offcpu/flamegraph");
	ASSERT_TRUE(res);
	EXPECT_EQ(res->status, 200);
}

TEST(QueryHTTP, RateLimitsAPI) {
	storage::memory::Store store(storage::memory::Options{Duration(0)});
	query::Service svc(store);
	query::Handler h(svc, nullptr,
	                 query::HandlerOptions{query::HandlerConfig{7 * 24h, 15min, 24h, 4, 100},
	                                       query::AuthConfig{.allow_unauthenticated = true},
	                                       ratelimit::Policy{.enabled = true, .rate = 0.01, .burst = 1}, false},
	                 log::Logger::discard());
	TestServer r([&](httplib::Server& s) { h.routes(s); });
	httplib::Client c(r.url());

	auto res = c.Get("/api/v1/offcpu/flamegraph");
	ASSERT_TRUE(res);
	EXPECT_EQ(res->status, 200);
	res = c.Get("/api/v1/offcpu/flamegraph");
	ASSERT_TRUE(res);
	EXPECT_EQ(res->status, 429);
	EXPECT_EQ(res->get_header_value("Retry-After"), "1");
}

TEST(CollectorAuth, FailsClosed) {
	EXPECT_THROW(collector::Auth({}, false, false), Error);
	EXPECT_THROW(collector::Auth({""}, false, false), Error);
	EXPECT_NO_THROW(collector::Auth({"token"}, false, false));
	EXPECT_NO_THROW(collector::Auth({}, true, false));
	EXPECT_NO_THROW(collector::Auth({}, false, true));

	collector::Auth auth({"token"}, false, false);
	EXPECT_TRUE(auth.authorize("Bearer token"));
	EXPECT_FALSE(auth.authorize("Bearer wrong"));
	EXPECT_FALSE(auth.authorize("Basic dG9rZW46"));
}

TEST(Wire, RejectsOversizedStacksDuringDecode) {
	etrace::v1::Batch p;
	p.set_id("b");
	p.mutable_resource()->set_node("n");
	auto* sample = p.add_offcpu();
	for (int i = 0; i < 32; i++) {
		sample->mutable_stack()->add_frames()->set_function("f");
	}
	wire::Limits limits;
	limits.max_frames = 8;
	EXPECT_THROW(wire::from_proto(p, limits), wire::InvalidBatch);
	EXPECT_NO_THROW(wire::from_proto(p));
}

model::Batch pyro_batch() {
	model::Batch b;
	b.resource = model::Resource{"n1", "", "", {{"cluster", "c1"}}};
	b.window_start = etrace::now() - 1s;
	b.window_end = etrace::now();
	model::PodRef pod{"", "api-0", "shop", ""};
	b.offcpu = {
	        model::OffCPUSample{model::Task{0, 0, "api", "", pod}, testing::frames({"futex_wait"}), {}, 7, 1, {}},
	        model::OffCPUSample{model::Task{0, 0, "api", "", pod}, testing::frames({"read"}), {}, 3, 1, {}},
	        model::OffCPUSample{model::Task{0, 0, "db", "", {}}, testing::frames({"fsync"}), {}, 5, 1, {}},
	};
	return b;
}

std::string gunzip(const std::string& in) {
	z_stream zs{};
	inflateInit2(&zs, 15 + 16);
	std::string out(1 << 20, '\0');
	zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
	zs.avail_in = static_cast<uInt>(in.size());
	zs.next_out = reinterpret_cast<Bytef*>(out.data());
	zs.avail_out = static_cast<uInt>(out.size());
	inflate(&zs, Z_FINISH);
	out.resize(zs.total_out);
	inflateEnd(&zs);
	return out;
}

std::string base64_decode(const std::string& in) {
	static const std::string tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	uint32_t val = 0;
	int bits = -8;
	for (char c : in) {
		auto p = tbl.find(c);
		if (p == std::string::npos) {
			break;
		}
		val = (val << 6) | static_cast<uint32_t>(p);
		bits += 6;
		if (bits >= 0) {
			out += static_cast<char>((val >> bits) & 0xff);
			bits -= 8;
		}
	}
	return out;
}

TEST(Pyroscope, ExportGroupsSeriesPerTask) {
	nlohmann::json got;
	TestServer r([&](httplib::Server& s) {
		s.Post("/push.v1.PusherService/Push", [&](const httplib::Request& req, httplib::Response& res) {
			if (req.get_header_value("X-Scope-OrgID") != "t1") {
				res.status = 400;
				return;
			}
			got = nlohmann::json::parse(req.body);
		});
	});
	agent::pyroscope::Options o;
	o.url = r.url();
	o.tenant_id = "t1";
	o.validate();
	agent::pyroscope::Exporter(o).export_batch({}, pyro_batch());
	ASSERT_EQ(got["series"].size(), 2u);
	std::map<std::string, std::string> labels;
	for (const auto& l : got["series"][0]["labels"]) {
		labels[l["name"]] = l["value"];
	}
	EXPECT_EQ(labels["__name__"], "offcpu");
	EXPECT_EQ(labels["service_name"], "etrace");
	EXPECT_EQ(labels["namespace"], "shop");
	EXPECT_EQ(labels["cluster"], "c1");
	auto raw = gunzip(base64_decode(got["series"][0]["samples"][0]["rawProfile"]));
	EXPECT_NE(raw.find("futex_wait"), std::string::npos);
	EXPECT_NE(raw.find("off_cpu"), std::string::npos);
}

// GPU waits go to their own profile type so they do not add to off-CPU time.
TEST(Pyroscope, GpuWaitsSeparateProfile) {
	auto b = pyro_batch();
	b.offcpu.push_back(model::OffCPUSample{model::Task{0, 0, "db", "", {}},
	                                       testing::frames({"cuStreamSynchronize"}),
	                                       {},
	                                       9,
	                                       1,
	                                       model::OffCPUReason::gpu});
	agent::pyroscope::Options o;
	o.url = "http://127.0.0.1:1";
	auto got = nlohmann::json::parse(agent::pyroscope::Exporter(o).build_body(b));
	ASSERT_EQ(got["series"].size(), 3u);
	int gpu = 0;
	for (const auto& s : got["series"]) {
		std::string name;
		for (const auto& l : s["labels"]) {
			if (l["name"] == "__name__") {
				name = l["value"];
			}
		}
		auto raw = gunzip(base64_decode(s["samples"][0]["rawProfile"]));
		bool has_sync = raw.find("cuStreamSynchronize") != std::string::npos;
		if (name == "gpu") {
			gpu++;
			EXPECT_TRUE(has_sync);
			EXPECT_NE(raw.find("gpu_wait"), std::string::npos);
		} else {
			EXPECT_EQ(name, "offcpu");
			EXPECT_FALSE(has_sync) << "GPU time leaked into the off-CPU profile";
		}
	}
	EXPECT_EQ(gpu, 1);
}

TEST(Pyroscope, ErrorClasses) {
	for (auto [code, permanent] :
	     std::vector<std::pair<int, bool>>{{400, true}, {401, true}, {429, false}, {503, false}}) {
		TestServer r([code](httplib::Server& s) {
			s.Post("/push.v1.PusherService/Push",
			       [code](const httplib::Request&, httplib::Response& res) { res.status = code; });
		});
		agent::pyroscope::Options o;
		o.url = r.url();
		agent::pyroscope::Exporter e(o);
		try {
			e.export_batch({}, pyro_batch());
			ADD_FAILURE() << code << ": no error";
		} catch (const agent::PermanentError&) {
			EXPECT_TRUE(permanent) << code;
		} catch (const Error&) {
			EXPECT_FALSE(permanent) << code;
		}
	}
	agent::pyroscope::Options o;
	o.url = "http://127.0.0.1:1";
	EXPECT_NO_THROW(agent::pyroscope::Exporter(o).export_batch({}, model::Batch{}))
	        << "empty batch must not be sent";
	o.url = "ftp://x";
	EXPECT_THROW(o.validate(), Error);
}

const std::vector<uint8_t> kTestID = {0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89};
const std::string kTestIDHex = "abcdef0123456789";
const std::vector<testing::TestSym> kWorkSym = {{"work", 0x1000, 0x100}};

symbol::DebuginfodConfig debuginfod_config(const testing::TempDir& dir, std::vector<std::string> urls) {
	symbol::DebuginfodConfig c;
	c.urls = std::move(urls);
	c.cache_dir = dir.path();
	c.timeout = 5s;
	c.validate();
	return c;
}

TEST(Debuginfod, FetchesFromFirstServerThatHasIt) {
	testing::TempDir src, cache;
	testing::write_elf(src / "src.debug", kTestID, kWorkSym);
	auto body = testing::read_file(src / "src.debug");
	std::atomic<int> miss_hits{0}, hits{0};
	TestServer empty([&](httplib::Server& s) {
		s.Get(".*", [&](const httplib::Request&, httplib::Response& res) {
			miss_hits++;
			res.status = 404;
		});
	});
	TestServer full([&](httplib::Server& s) {
		s.Get(".*", [&](const httplib::Request& req, httplib::Response& res) {
			hits++;
			if (req.path != "/buildid/" + kTestIDHex + "/debuginfo") {
				res.status = 404;
				return;
			}
			res.set_content(body, "application/octet-stream");
		});
	});
	telemetry::Registry reg(false);
	symbol::Debuginfod d(debuginfod_config(cache, {empty.url(), full.url() + "/"}), log::Logger::discard(), reg);
	EXPECT_FALSE(d.find(kTestIDHex)) << "found before download";
	std::optional<std::string> path;
	ASSERT_TRUE(testing::wait_for([&] { return (path = d.find(kTestIDHex)).has_value(); }));
	auto f = symbol::ElfFile::open(*path);
	EXPECT_EQ(symbol::build_id(*f), kTestIDHex);
	EXPECT_EQ(miss_hits.load(), 1);
	EXPECT_EQ(hits.load(), 1);
}

TEST(Debuginfod, RemembersFailuresAndRejectsBadIDs) {
	testing::TempDir cache;
	std::atomic<int> hits{0};
	TestServer srv([&](httplib::Server& s) {
		s.Get(".*", [&](const httplib::Request&, httplib::Response& res) {
			hits++;
			res.status = 404;
		});
	});
	telemetry::Registry reg(false);
	symbol::Debuginfod d(debuginfod_config(cache, {srv.url()}), log::Logger::discard(), reg);
	d.find(kTestIDHex);
	ASSERT_TRUE(testing::wait_for(
	        [&] { return testing::metric(reg, "etrace_debuginfod_fetches_total", "result", "not_found") == 1; }));
	std::this_thread::sleep_for(50ms);
	d.find(kTestIDHex);
	std::this_thread::sleep_for(50ms);
	EXPECT_EQ(hits.load(), 1) << "server asked again within retry_after";
	EXPECT_FALSE(d.find("../../etc/passwd"));
}

TEST(Debuginfod, RejectsBadDownloads) {
	testing::TempDir src, cache;
	testing::write_elf(src / "wrong.debug", {9, 9, 9, 9}, kWorkSym);
	auto wrong = testing::read_file(src / "wrong.debug");
	TestServer srv([&](httplib::Server& s) {
		s.Get(".*", [&](const httplib::Request& req, httplib::Response& res) {
			if (req.path == "/buildid/" + kTestIDHex + "/debuginfo") {
				res.set_content(wrong, "application/octet-stream");
			} else {
				res.set_content(std::string(4096, 'x'), "application/octet-stream");
			}
		});
	});
	auto cfg = debuginfod_config(cache, {srv.url()});
	cfg.max_file_bytes = 2048;
	cfg.cache_max_bytes = 2048;
	telemetry::Registry reg(false);
	symbol::Debuginfod d(cfg, log::Logger::discard(), reg);
	d.find(kTestIDHex);
	d.find("0011223344");
	ASSERT_TRUE(testing::wait_for([&] {
		return testing::metric(reg, "etrace_debuginfod_fetches_total", "result", "invalid") == 1 &&
		       testing::metric(reg, "etrace_debuginfod_fetches_total", "result", "too_large") == 1;
	}));
	EXPECT_TRUE(std::filesystem::is_empty(cache.path())) << "rejected downloads left files";
}

TEST(Debuginfod, ConfigValidation) {
	symbol::DebuginfodConfig c;
	EXPECT_NO_THROW(c.validate()) << "disabled config must be valid";
	c.urls = {"file:///etc"};
	EXPECT_THROW(c.validate(), Error);
}

// The agent's gRPC exporter delivers to a collector that stores into
// memory; a retried push is idempotent and an authentication failure is
// retried instead of dropped.
TEST(Ingest, AgentToCollectorToQuery) {
	storage::memory::Store store(storage::memory::Options{Duration(0)});
	telemetry::Registry reg(false);
	collector::Service svc(store, collector::DedupConfig{100, 1min}, collector::Limits{0, 0, 0, 0}, reg,
	                       log::Logger::discard());
	collector::GrpcHandler handler(svc, collector::Auth({"secret"}, false, false), wire::Limits{},
	                               log::Logger::discard());
	grpcx::ServerConfig sc;
	sc.listen = "127.0.0.1:0";
	sc.graceful_timeout = 1s;
	grpcx::Server srv(sc, log::Logger::discard());
	srv.register_service(&handler);
	std::stop_source stop;
	std::thread t([&] { srv.run(stop.get_token()); });
	ASSERT_TRUE(testing::wait_for([&] { return srv.bound_port() != 0; }));

	agent::grpcexp::Options opts;
	opts.target = "127.0.0.1:" + std::to_string(srv.bound_port());
	opts.token = "secret";
	opts.tls.allow_insecure = true;
	agent::grpcexp::Exporter exp(opts);
	auto now = etrace::now();
	model::Batch b;
	b.id = "b1";
	b.resource.node = "node-a";
	b.window_start = now - 10s;
	b.window_end = now;
	b.offcpu.push_back(model::OffCPUSample{model::Task{7, 7, "db", "", {}},
	                                       {model::Frame{0, "io_schedule", "", model::FrameKind::kernel}},
	                                       {},
	                                       static_cast<uint64_t>(Duration(5ms).count()),
	                                       2,
	                                       {}});
	EXPECT_NO_THROW(exp.export_batch({}, b));
	EXPECT_NO_THROW(exp.export_batch({}, b)) << "retried export must be idempotent";

	query::Service qs(store);
	storage::Filter f;
	f.node = "node-a";
	auto lines = qs.lines(query::FlameRequest{f, 0, 0});
	ASSERT_EQ(lines.size(), 1u);
	EXPECT_EQ(lines[0].value, static_cast<uint64_t>(Duration(5ms).count()));

	auto bad = opts;
	bad.token = "wrong";
	try {
		agent::grpcexp::Exporter(bad).export_batch({}, b);
		FAIL() << "a wrong token must fail";
	} catch (const agent::PermanentError&) {
		FAIL() << "an authentication failure must be retried, not dropped";
	} catch (const Error&) {
	}
	stop.request_stop();
	t.join();
}

// A batch over max_send_msg_bytes is delivered in parts, a retry after it is
// deduplicated, and an item too large to send on its own is dropped
// permanently instead of blocking the batch forever.
TEST(Ingest, SplitsOversizedBatch) {
	storage::memory::Store store(storage::memory::Options{Duration(0)});
	telemetry::Registry reg(false);
	collector::Service svc(store, collector::DedupConfig{100, 1min}, collector::Limits{0, 0, 0, 0}, reg,
	                       log::Logger::discard());
	collector::GrpcHandler handler(svc, collector::Auth({}, true, false), wire::Limits{}, log::Logger::discard());
	grpcx::ServerConfig sc;
	sc.listen = "127.0.0.1:0";
	sc.graceful_timeout = 1s;
	grpcx::Server srv(sc, log::Logger::discard());
	srv.register_service(&handler);
	std::stop_source stop;
	std::thread t([&] { srv.run(stop.get_token()); });
	ASSERT_TRUE(testing::wait_for([&] { return srv.bound_port() != 0; }));

	agent::grpcexp::Options opts;
	opts.target = "127.0.0.1:" + std::to_string(srv.bound_port());
	opts.max_send_msg_bytes = 4096;
	agent::grpcexp::Exporter exp(opts);
	auto now = etrace::now();
	model::Batch b;
	b.id = "big";
	b.resource.node = "node-a";
	b.window_start = now - 10s;
	b.window_end = now;
	auto sample = [](std::string fn) {
		return model::OffCPUSample{model::Task{7, 7, "db", "", {}},
		                           {model::Frame{0, std::move(fn), "", model::FrameKind::kernel}},
		                           {},
		                           static_cast<uint64_t>(Duration(1ms).count()),
		                           1,
		                           {}};
	};
	for (int i = 0; i < 300; i++) {
		b.offcpu.push_back(sample("io_schedule"));
	}
	b.offcpu.push_back(sample(std::string(8192, 'x')));

	EXPECT_THROW(exp.export_batch({}, b), agent::PermanentError) << "the oversized item must be reported";
	EXPECT_THROW(exp.export_batch({}, b), agent::PermanentError);

	query::Service qs(store);
	storage::Filter f;
	f.node = "node-a";
	auto lines = qs.lines(query::FlameRequest{f, 0, 0});
	ASSERT_EQ(lines.size(), 1u) << "only the item that fits alone is dropped";
	EXPECT_EQ(lines[0].value, static_cast<uint64_t>(Duration(300ms).count()))
	        << "retried parts must be deduplicated";

	b.offcpu.pop_back();
	b.id = "fits";
	EXPECT_NO_THROW(exp.export_batch({}, b));
	stop.request_stop();
	t.join();
}

// make_pki writes a CA, a server certificate for localhost and a client
// certificate with the openssl CLI, or returns false when it is missing.
bool make_pki(const testing::TempDir& d) {
	auto cmd = std::format(
	        "cd {} && openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=ca -keyout ca.key -out ca.pem && "
	        "openssl req -newkey rsa:2048 -nodes -subj /CN=localhost -keyout srv.key -out srv.csr && "
	        "printf 'subjectAltName=DNS:localhost,IP:127.0.0.1' > ext && "
	        "openssl x509 -req -in srv.csr -CA ca.pem -CAkey ca.key -CAcreateserial -days 1 -extfile ext -out "
	        "srv.pem && "
	        "openssl req -newkey rsa:2048 -nodes -subj /CN=agent -keyout cli.key -out cli.csr && "
	        "openssl x509 -req -in cli.csr -CA ca.pem -CAkey ca.key -CAcreateserial -days 1 -out cli.pem",
	        d.path());
	return std::system(("(" + cmd + ") >/dev/null 2>&1").c_str()) == 0;
}

TEST(Ingest, MutualTLS) {
	testing::TempDir pki;
	if (!make_pki(pki)) {
		GTEST_SKIP() << "openssl CLI not available";
	}
	storage::memory::Store store(storage::memory::Options{Duration(0)});
	telemetry::Registry reg(false);
	collector::Service svc(store, collector::DedupConfig{100, 1min}, collector::Limits{0, 0, 0, 0}, reg,
	                       log::Logger::discard());
	collector::GrpcHandler handler(svc, collector::Auth({}, false, true), wire::Limits{}, log::Logger::discard());
	grpcx::ServerConfig sc;
	sc.listen = "127.0.0.1:0";
	sc.graceful_timeout = 1s;
	sc.tls = tls::Config{true, pki / "srv.pem", pki / "srv.key", pki / "ca.pem", "", true, false};
	grpcx::Server srv(sc, log::Logger::discard());
	srv.register_service(&handler);
	std::stop_source stop;
	std::thread t([&] { srv.run(stop.get_token()); });
	ASSERT_TRUE(testing::wait_for([&] { return srv.bound_port() != 0; }));

	model::Batch b;
	b.id = "tls";
	b.resource.node = "n";
	b.offcpu.push_back({});
	agent::grpcexp::Options opts;
	opts.target = "127.0.0.1:" + std::to_string(srv.bound_port());
	opts.timeout = 5s;
	opts.tls = tls::Config{true, pki / "cli.pem", pki / "cli.key", pki / "ca.pem", "localhost", false, false};
	EXPECT_NO_THROW(agent::grpcexp::Exporter(opts).export_batch({}, b));

	auto skip = opts;
	skip.tls.ca_file.clear();
	skip.tls.insecure_skip_verify = true;
	b.id = "tls-skip";
	EXPECT_NO_THROW(agent::grpcexp::Exporter(skip).export_batch({}, b));

	auto anonymous = opts;
	anonymous.tls.cert_file.clear();
	anonymous.tls.key_file.clear();
	anonymous.timeout = 2s;
	b.id = "tls-anon";
	EXPECT_THROW(agent::grpcexp::Exporter(anonymous).export_batch({}, b), Error)
	        << "server must require a client certificate";
	stop.request_stop();
	t.join();
}

}  // namespace
}  // namespace etrace
