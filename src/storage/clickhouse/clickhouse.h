#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "platform/config.h"
#include "storage/storage.h"

namespace clickhouse {
class Client;
}

namespace etrace::storage::clickhouse {

struct Options {
	std::vector<std::string> addrs;
	std::string database = "etrace";
	std::string samples_table = "offcpu_samples";
	std::string events_table = "offcpu_events";
	std::string cluster;
	std::string username;
	std::string password;
	int ttl_days = 7;
	Duration dial_timeout = 5s;
	// query_timeout bounds each send and receive on a connection so a
	// stalled server cannot block a writer or reader forever.
	Duration query_timeout = 60s;
	// Server-side read budgets. max_execution_time is enforced by
	// ClickHouse; max_memory_usage and max_rows_to_group_by reject queries
	// that would exhaust a replica.
	Duration max_execution_time = 30s;
	uint64_t max_memory_usage = uint64_t{1} << 30;
	uint64_t max_rows_to_group_by = 2000000;
	int max_open_conns = 16;
	bool auto_migrate = true;

	void validate() const;
	std::string local_name(const std::string& t) const { return cluster.empty() ? t : t + "_local"; }
};

void decode_fields(config::Fields& f, Options& o);

std::string quote_ident(const std::string& s);
// schema_statements returns the idempotent DDL that creates or upgrades
// the database and tables.
std::vector<std::string> schema_statements(const Options& o);

// Store writes to and reads from ClickHouse over the native protocol, with
// a bounded pool of connections spread round robin over addrs.
class Store : public storage::Store {
public:
	explicit Store(Options o);
	~Store() override;

	// write_batch inserts into each table with a per-table deduplication
	// token, so a retry after a partial failure only fills in what is
	// missing.
	void write_batch(const model::Batch& b) override;
	std::vector<ChainValue> offcpu_chains(const ChainQuery& q) override;
	std::vector<Event> events(const EventQuery& q) override;
	void visit_events(const EventQuery& q, const EventVisitor& visit) override;
	void ping(Duration timeout) override;
	void close() override;

	void migrate();
	void execute(const std::string& sql);

private:
	class Lease;
	friend class Lease;
	std::unique_ptr<::clickhouse::Client> connect(Duration dial, Duration io);
	Lease acquire();
	std::string query_settings() const;
	void give_back(std::unique_ptr<::clickhouse::Client> c);

	Options opts_;
	std::string samples_;
	std::string events_;
	std::mutex mu_;
	std::condition_variable cv_;
	std::vector<std::unique_ptr<::clickhouse::Client>> idle_;
	int open_ = 0;
	bool closed_ = false;
};

}  // namespace etrace::storage::clickhouse
