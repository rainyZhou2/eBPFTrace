#include <gtest/gtest.h>

#include <cstdlib>
#include <sstream>

#include "platform/auth.h"
#include "platform/compress.h"
#include "platform/config.h"
#include "platform/flags.h"
#include "platform/lru.h"
#include "platform/ratelimit.h"
#include "platform/spool.h"
#include "platform/time.h"
#include "platform/url.h"
#include "testutil.h"

namespace etrace {
namespace {

TEST(Lru, EvictionAndTTL) {
	auto now = std::chrono::steady_clock::time_point{};
	LruCache<std::string, int> c(2, 1min, [&] { return now; });
	c.add("a", 1);
	c.add("b", 2);
	c.get("a");
	c.add("c", 3);
	EXPECT_FALSE(c.get("b")) << "b should be evicted as least recently used";
	EXPECT_EQ(c.get("a").value_or(0), 1);
	now += 2min;
	EXPECT_FALSE(c.get("a")) << "a should be expired";
	EXPECT_TRUE(c.add_if_absent("a", 5));
	EXPECT_FALSE(c.add_if_absent("a", 6));
}

TEST(Spool, FIFOAndRecovery) {
	testing::TempDir dir;
	{
		spool::Spool s(dir.path(), 0);
		for (const char* v : {"a", "b", "c"}) {
			s.put(v);
		}
		auto rec = s.peek();
		ASSERT_TRUE(rec);
		EXPECT_EQ(rec->data, "a");
		s.ack(rec->id);
	}
	spool::Spool reopened(dir.path(), 0);
	EXPECT_EQ(reopened.size(), 2u);
	EXPECT_EQ(reopened.peek()->data, "b");
}

TEST(Spool, EvictsOldestOverBudget) {
	testing::TempDir dir;
	spool::Spool s(dir.path(), 4);
	for (const char* v : {"aa", "bb", "cc"}) {
		s.put(v);
	}
	EXPECT_EQ(s.size(), 2u);
	EXPECT_EQ(s.evicted(), 1u);
	auto rec = s.peek();
	EXPECT_EQ(rec->data, "bb");
	s.ack(rec->id);
	s.ack(s.peek()->id);
	EXPECT_FALSE(s.peek());
}

TEST(Spool, RemovesPartialWrites) {
	testing::TempDir dir;
	testing::write_file(dir / "x.rec.tmp", "partial");
	spool::Spool s(dir.path(), 0);
	EXPECT_EQ(s.size(), 0u);
	EXPECT_FALSE(std::filesystem::exists(dir / "x.rec.tmp"));
}

TEST(Config, ExpandsEnvironmentAfterParsing) {
	setenv("ETRACE_TEST_VALUE", "a: b#c", 1);
	auto node = config::parse_text(
	        "plain: ${ETRACE_TEST_VALUE}\nshort: $ETRACE_TEST_VALUE\nescaped: $${ETRACE_TEST_VALUE}\n");
	EXPECT_EQ(node["plain"].as<std::string>(), "a: b#c");
	EXPECT_EQ(node["short"].as<std::string>(), "a: b#c");
	EXPECT_EQ(node["escaped"].as<std::string>(), "${ETRACE_TEST_VALUE}");

	unsetenv("ETRACE_TEST_MISSING");
	auto missing = config::parse_text("v: ${ETRACE_TEST_MISSING}\n")["v"];
	EXPECT_TRUE(missing.IsNull() || missing.as<std::string>().empty());
	EXPECT_THROW(config::parse_text("v: ${ETRACE_TEST_MISSING:?missing value}\n"), Error);
	unsetenv("ETRACE_TEST_VALUE");
}

TEST(Auth, TokenAndBasicHeaders) {
	EXPECT_TRUE(auth::constant_time_equal("abc", "abc"));
	EXPECT_FALSE(auth::constant_time_equal("abc", "abd"));
	EXPECT_FALSE(auth::constant_time_equal("abc", "ab"));

	auto token = auth::bearer_token("Bearer secret");
	ASSERT_TRUE(token);
	EXPECT_EQ(*token, "secret");
	EXPECT_FALSE(auth::bearer_token("Basic c2VjcmV0"));

	auto basic = auth::basic_credentials("Basic b3BzOnB3");
	ASSERT_TRUE(basic);
	EXPECT_EQ(basic->user, "ops");
	EXPECT_EQ(basic->password, "pw");
	EXPECT_FALSE(auth::basic_credentials("Basic !!!"));

	auth::TokenSet tokens({"one", "two"});
	EXPECT_TRUE(tokens.contains("two"));
	EXPECT_FALSE(tokens.contains("three"));
}

TEST(RateLimit, TokenBucket) {
	std::chrono::steady_clock::time_point now{};
	ratelimit::Limiter limiter(ratelimit::Policy{true, 1.0, 1.0, 1min}, [&] { return now; });
	EXPECT_TRUE(limiter.allow("client"));
	EXPECT_FALSE(limiter.allow("client"));
	now += 1s;
	EXPECT_TRUE(limiter.allow("client"));
	EXPECT_TRUE(limiter.allow("other"));
	now += 2min;
	EXPECT_TRUE(limiter.allow("client"));
}

TEST(Compress, GzipNegotiation) {
	EXPECT_TRUE(compress::accepts_gzip("gzip"));
	EXPECT_TRUE(compress::accepts_gzip("br, gzip;q=0.5"));
	EXPECT_TRUE(compress::accepts_gzip("*"));
	EXPECT_FALSE(compress::accepts_gzip("gzip;q=0"));
	EXPECT_FALSE(compress::accepts_gzip("gzip;q=0, *;q=1"));
	EXPECT_FALSE(compress::accepts_gzip("identity"));
	EXPECT_FALSE(compress::accepts_gzip(""));
}

TEST(Duration, ParseAndFormatLikeGo) {
	EXPECT_EQ(parse_duration("1m30s"), 90s);
	EXPECT_EQ(parse_duration("1.5h"), 90min);
	EXPECT_EQ(parse_duration("250ms"), 250ms);
	EXPECT_EQ(parse_duration("10us"), 10us);
	EXPECT_EQ(parse_duration("-2s"), -2s);
	EXPECT_EQ(parse_duration("0"), 0s);
	EXPECT_THROW(parse_duration("10"), Error);
	EXPECT_THROW(parse_duration("1x"), Error);
	EXPECT_THROW(parse_duration(""), Error);
	EXPECT_EQ(format_duration(168h), "168h0m0s");
	EXPECT_EQ(format_duration(15min), "15m0s");
	EXPECT_EQ(format_duration(1500ms), "1.5s");
	EXPECT_EQ(format_duration(250us), "250\xc2\xb5s");
	EXPECT_EQ(format_duration(0s), "0s");
}

TEST(Time, RFC3339) {
	auto t = parse_rfc3339("2024-05-01T10:00:00.25+02:00");
	ASSERT_TRUE(t);
	EXPECT_EQ(format_rfc3339nano(*t), "2024-05-01T08:00:00.25Z");
	EXPECT_FALSE(parse_rfc3339("2024-05-01 10:00:00"));
	EXPECT_FALSE(parse_rfc3339("2024-13-01T10:00:00Z"));
}

struct Inner {
	std::string name = "def";
	Duration wait = 5s;
	void validate() const {
		if (name == "bad") {
			throw Error("name must not be bad");
		}
	}
};
void decode_fields(config::Fields& f, Inner& i) {
	f("name", i.name)("wait", i.wait);
}

struct Outer {
	Inner inner;
	std::vector<std::string> list;
	std::map<std::string, std::string> labels;
	config::Plugin plugin;
};
void decode_fields(config::Fields& f, Outer& o) {
	f("inner", o.inner)("list", o.list)("labels", o.labels)("plugin", o.plugin);
}

TEST(Config, DecodesStrictlyWithDefaultsAndEnv) {
	setenv("ETRACE_TEST_NAME", "from-env", 1);
	auto o = config::parse<Outer>("inner: {name: ${ETRACE_TEST_NAME}}\nlist: [a, b]\nlabels: {k: v, empty: }\n");
	EXPECT_EQ(o.inner.name, "from-env");
	EXPECT_EQ(o.inner.wait, 5s);
	EXPECT_EQ(o.list, (std::vector<std::string>{"a", "b"}));
	EXPECT_EQ(o.labels.at("empty"), "");
	EXPECT_THROW(config::parse<Outer>("inner: {nam: x}\n"), Error);
	EXPECT_THROW(config::parse<Outer>("inner: {name: bad}\n"), Error);
	EXPECT_THROW(config::parse<Outer>("inner: {wait: soon}\n"), Error);
}

TEST(Config, PluginOptionsRejectUnknownFields) {
	auto o = config::parse<Outer>("plugin: {driver: offcpu, options: {waker_stacks: true}}\n");
	EXPECT_EQ(o.plugin.driver, "offcpu");
	EXPECT_THROW(o.plugin.options_as<Inner>(), Error);
	auto none = config::parse<Outer>("plugin: {driver: x}\n");
	EXPECT_EQ(none.plugin.options_as<Inner>().name, "def");
}

TEST(Flags, GoStyle) {
	std::ostringstream err;
	flags::FlagSet fs("t", err);
	Duration d = 30s;
	uint32_t pid = 0;
	bool v = false;
	std::string out = "x";
	fs.duration("duration", d, "");
	fs.unsigned32("pid", pid, "");
	fs.boolean("v", v, "");
	fs.string("o", out, "");
	auto rest = fs.parse({"-duration", "5s", "--pid=42", "-v", "-o", "", "tail"});
	EXPECT_EQ(d, 5s);
	EXPECT_EQ(pid, 42u);
	EXPECT_TRUE(v);
	EXPECT_EQ(out, "");
	EXPECT_EQ(rest, std::vector<std::string>{"tail"});
	EXPECT_THROW(fs.parse({"-nope"}), Error);
	EXPECT_THROW(fs.parse({"-h"}), flags::HelpRequested);
}

TEST(Url, ParsesHttpOnly) {
	auto u = parse_http_url("https://h:8443/base/");
	ASSERT_TRUE(u);
	EXPECT_EQ(u->origin(), "https://h:8443");
	EXPECT_EQ(u->path, "/base");
	EXPECT_FALSE(parse_http_url("ftp://x"));
	EXPECT_FALSE(parse_http_url("http://"));
	EXPECT_EQ(base64_encode("ab"), "YWI=");
}

}  // namespace
}  // namespace etrace
