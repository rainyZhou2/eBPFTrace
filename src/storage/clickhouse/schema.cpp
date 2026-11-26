#include <format>
#include <regex>
#include <string>
#include <vector>

#include "storage/clickhouse/clickhouse.h"

namespace etrace::storage::clickhouse {

namespace {

constexpr const char* kChainColumns = R"(	chain_comm Array(LowCardinality(String)),
	chain_pid Array(UInt32),
	chain_wake_stack Array(Array(String)),
	chain_wait_stack Array(Array(String)),
	chain_wait_ns Array(UInt64),
	chain_trace_id Array(String),
	chain_span_id Array(String),)";

struct TableDef {
	std::string name;
	std::string columns;
	std::string order;
	std::string extra;
};

std::vector<TableDef> tables(const Options& o) {
	std::string chain = kChainColumns;
	return {
	        {o.samples_table, std::string(R"(	batch_id String,
	window_start DateTime64(9, 'UTC'),
	window_end DateTime64(9, 'UTC'),
	node LowCardinality(String),
	agent_id String,
	labels Map(LowCardinality(String), String),
	pid UInt32,
	tid UInt32,
	comm LowCardinality(String),
	container_id String,
	reason LowCardinality(String),
	stack Array(String),
)") + chain + R"(
	blocked_ns UInt64,
	count UInt64)",
	         "PARTITION BY toDate(window_start) ORDER BY (node, comm, window_start)",
	         std::format("TTL toDateTime(window_start) + INTERVAL {} DAY", o.ttl_days)},
	        {o.events_table, std::string(R"(	batch_id String,
	node LowCardinality(String),
	agent_id String,
	labels Map(LowCardinality(String), String),
	trace_id String,
	span_id String,
	start_time DateTime64(9, 'UTC'),
	end_time DateTime64(9, 'UTC'),
	duration_ns UInt64,
	pid UInt32,
	tid UInt32,
	comm LowCardinality(String),
	container_id String,
	reason LowCardinality(String),
	stack Array(String),
)") + chain.substr(0, chain.size() - 1) + R"(,
	INDEX idx_trace trace_id TYPE bloom_filter(0.01) GRANULARITY 4)",
	         "PARTITION BY toDate(start_time) ORDER BY (node, start_time)",
	         std::format("TTL toDateTime(start_time) + INTERVAL {} DAY", o.ttl_days)},
	};
}

// The added columns were introduced after the first schema. They are added
// with ALTER so existing tables migrate in place; writers name every
// column, so their position at the end of the table does not matter.
constexpr const char* kAddColumns =
        "ADD COLUMN IF NOT EXISTS pod_uid String DEFAULT '', "
        "ADD COLUMN IF NOT EXISTS pod LowCardinality(String) DEFAULT '', "
        "ADD COLUMN IF NOT EXISTS namespace LowCardinality(String) DEFAULT '', "
        "ADD COLUMN IF NOT EXISTS container LowCardinality(String) DEFAULT ''";
constexpr const char* kAddEventColumns =
        ", ADD COLUMN IF NOT EXISTS gpu_stream UInt64 DEFAULT 0, "
        "ADD COLUMN IF NOT EXISTS gpu_launches UInt32 DEFAULT 0";

bool ident(const std::string& s) {
	static const std::regex re("^[A-Za-z_][A-Za-z0-9_]*$");
	return std::regex_match(s, re);
}

}  // namespace

std::string quote_ident(const std::string& s) {
	std::string out = "`";
	for (char c : s) {
		if (c == '`') {
			out += "``";
		} else {
			out += c;
		}
	}
	return out + "`";
}

void Options::validate() const {
	if (addrs.empty()) {
		throw Error("addrs is required");
	}
	for (const auto& a : addrs) {
		if (a.empty()) {
			throw Error("addrs must not contain empty entries");
		}
	}
	for (const auto* id : {&database, &samples_table, &events_table}) {
		if (!ident(*id)) {
			throw Error(std::format("invalid identifier \"{}\"", *id));
		}
	}
	if (!cluster.empty() && !ident(cluster)) {
		throw Error(std::format("invalid cluster \"{}\"", cluster));
	}
	if (ttl_days <= 0) {
		throw Error("ttl_days must be positive");
	}
	if (max_open_conns <= 0) {
		throw Error("max_open_conns must be positive");
	}
	if (dial_timeout <= Duration(0) || query_timeout <= Duration(0)) {
		throw Error("dial_timeout and query_timeout must be positive");
	}
	if (max_execution_time <= Duration(0)) {
		throw Error("max_execution_time must be positive");
	}
	if (max_memory_usage == 0 || max_rows_to_group_by == 0) {
		throw Error("max_memory_usage and max_rows_to_group_by must be positive");
	}
}

void decode_fields(config::Fields& f, Options& o) {
	f("addrs", o.addrs)("database", o.database)("samples_table", o.samples_table)("events_table", o.events_table)(
	        "cluster", o.cluster)("username", o.username)("password", o.password)("ttl_days", o.ttl_days)(
	        "dial_timeout", o.dial_timeout)("query_timeout", o.query_timeout)(
	        "max_execution_time", o.max_execution_time)("max_memory_usage", o.max_memory_usage)(
	        "max_rows_to_group_by", o.max_rows_to_group_by)("max_open_conns", o.max_open_conns)("auto_migrate",
	                                                                                            o.auto_migrate);
}

std::vector<std::string> schema_statements(const Options& o) {
	std::string on_cluster, engine = "MergeTree", settings = "SETTINGS non_replicated_deduplication_window = 10000";
	if (!o.cluster.empty()) {
		on_cluster = " ON CLUSTER " + quote_ident(o.cluster);
		engine = "ReplicatedMergeTree";
		settings.clear();
	}
	auto db = quote_ident(o.database);
	std::vector<std::string> stmts{std::format("CREATE DATABASE IF NOT EXISTS {}{}", db, on_cluster)};
	for (const auto& t : tables(o)) {
		auto local = db + "." + quote_ident(o.local_name(t.name));
		std::string add = kAddColumns;
		if (t.name == o.events_table) {
			add += kAddEventColumns;
		}
		stmts.push_back(std::format("CREATE TABLE IF NOT EXISTS {}{} (\n{}\n) ENGINE = {}\n{}\n{}\n{}", local,
		                            on_cluster, t.columns, engine, t.order, t.extra, settings));
		if (!o.cluster.empty()) {
			stmts.push_back(
			        std::format("CREATE TABLE IF NOT EXISTS {}.{}{} AS {} ENGINE = Distributed({}, {}, {}, "
			                    "cityHash64(node))",
			                    db, quote_ident(t.name), on_cluster, local, quote_ident(o.cluster), db,
			                    quote_ident(o.local_name(t.name))));
		}
		stmts.push_back(std::format("ALTER TABLE {}{} {}", local, on_cluster, add));
		if (!o.cluster.empty()) {
			stmts.push_back(
			        std::format("ALTER TABLE {}.{}{} {}", db, quote_ident(t.name), on_cluster, add));
		}
	}
	return stmts;
}

}  // namespace etrace::storage::clickhouse
