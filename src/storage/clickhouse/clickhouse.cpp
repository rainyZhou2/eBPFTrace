#include "storage/clickhouse/clickhouse.h"

#include <clickhouse/client.h>

#include <format>
#include <map>

namespace ch = ::clickhouse;

namespace etrace::storage::clickhouse {

namespace {

using LcString = ch::ColumnLowCardinalityT<ch::ColumnString>;

std::string sql_string(std::string_view s) {
	std::string out = "'";
	for (char c : s) {
		if (c == '\'' || c == '\\') {
			out += '\\';
		}
		out += c;
	}
	return out + "'";
}

ch::Endpoint endpoint(const std::string& addr) {
	auto colon = addr.rfind(':');
	auto bracket = addr.find(']');
	if (colon == std::string::npos || (bracket != std::string::npos && bracket > colon)) {
		return ch::Endpoint{addr, 9000};
	}
	auto host = addr.substr(0, colon);
	if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
		host = host.substr(1, host.size() - 2);
	}
	return ch::Endpoint{host, static_cast<uint16_t>(std::stoi(addr.substr(colon + 1)))};
}

std::shared_ptr<ch::ColumnArray> array_of(const ch::ColumnRef& col) {
	auto a = col->As<ch::ColumnArray>();
	if (!a) {
		throw Error(std::format("clickhouse: column type {} is not an array", col->Type()->GetName()));
	}
	return a;
}

std::vector<std::string> strings_at(const ch::ColumnRef& col, size_t row) {
	auto items = array_of(col)->GetAsColumn(row);
	std::vector<std::string> out;
	if (auto s = items->As<ch::ColumnString>()) {
		out.reserve(s->Size());
		for (size_t i = 0; i < s->Size(); i++) {
			out.emplace_back(s->At(i));
		}
	} else if (auto lc = items->As<LcString>()) {
		for (size_t i = 0; i < lc->Size(); i++) {
			out.emplace_back(lc->At(i));
		}
	} else {
		throw Error(std::format("clickhouse: column type {} is not a string array", col->Type()->GetName()));
	}
	return out;
}

std::vector<std::vector<std::string>> nested_at(const ch::ColumnRef& col, size_t row) {
	auto outer = array_of(col)->GetAsColumn(row);
	std::vector<std::vector<std::string>> out;
	for (size_t i = 0; i < outer->Size(); i++) {
		out.push_back(strings_at(outer, i));
	}
	return out;
}

template <class T>
std::vector<T> numbers_at(const ch::ColumnRef& col, size_t row) {
	auto items = array_of(col)->GetAsColumn(row)->As<ch::ColumnVector<T>>();
	if (!items) {
		throw Error(std::format("clickhouse: column type {} has unexpected elements", col->Type()->GetName()));
	}
	std::vector<T> out;
	for (size_t i = 0; i < items->Size(); i++) {
		out.push_back(items->At(i));
	}
	return out;
}

// string_at reads String and LowCardinality(String) columns alike; string
// functions applied to a LowCardinality column keep it LowCardinality.
std::string string_at(const ch::ColumnRef& col, size_t row) {
	if (auto s = col->As<ch::ColumnString>()) {
		return std::string(s->At(row));
	}
	if (auto lc = col->As<LcString>()) {
		return std::string(lc->At(row));
	}
	throw Error(std::format("clickhouse: column type {} is not a string", col->Type()->GetName()));
}

template <class T>
T number_at(const ch::ColumnRef& col, size_t row) {
	auto c = col->As<ch::ColumnVector<T>>();
	if (!c) {
		throw Error(std::format("clickhouse: unexpected column type {}", col->Type()->GetName()));
	}
	return c->At(row);
}

// ChainColumns holds the parallel arrays a causal chain is stored in.
struct ChainColumns {
	std::shared_ptr<ch::ColumnArrayT<LcString>> comm = std::make_shared<ch::ColumnArrayT<LcString>>();
	std::shared_ptr<ch::ColumnArrayT<ch::ColumnUInt32>> pid =
	        std::make_shared<ch::ColumnArrayT<ch::ColumnUInt32>>();
	std::shared_ptr<ch::ColumnArrayT<ch::ColumnArrayT<ch::ColumnString>>> wake =
	        std::make_shared<ch::ColumnArrayT<ch::ColumnArrayT<ch::ColumnString>>>();
	std::shared_ptr<ch::ColumnArrayT<ch::ColumnArrayT<ch::ColumnString>>> wait =
	        std::make_shared<ch::ColumnArrayT<ch::ColumnArrayT<ch::ColumnString>>>();
	std::shared_ptr<ch::ColumnArrayT<ch::ColumnUInt64>> wait_ns =
	        std::make_shared<ch::ColumnArrayT<ch::ColumnUInt64>>();
	std::shared_ptr<ch::ColumnArrayT<ch::ColumnString>> trace =
	        std::make_shared<ch::ColumnArrayT<ch::ColumnString>>();
	std::shared_ptr<ch::ColumnArrayT<ch::ColumnString>> span =
	        std::make_shared<ch::ColumnArrayT<ch::ColumnString>>();

	void append(const std::vector<model::Hop>& c) {
		std::vector<std::string> comms, traces, spans;
		std::vector<uint32_t> pids;
		std::vector<std::vector<std::string>> wakes, waits;
		std::vector<uint64_t> ns;
		for (const auto& h : c) {
			comms.push_back(h.task.comm);
			pids.push_back(h.task.pid);
			wakes.push_back(model::symbols(h.wake_stack));
			waits.push_back(model::symbols(h.wait_stack));
			ns.push_back(h.wait_ns);
			traces.push_back(h.trace.trace_id_hex());
			spans.push_back(h.trace.span_id_hex());
		}
		comm->Append(comms);
		pid->Append(pids);
		wake->Append(wakes);
		wait->Append(waits);
		wait_ns->Append(ns);
		trace->Append(traces);
		span->Append(spans);
	}

	void add_to(ch::Block& b) const {
		b.AppendColumn("chain_comm", comm);
		b.AppendColumn("chain_pid", pid);
		b.AppendColumn("chain_wake_stack", wake);
		b.AppendColumn("chain_wait_stack", wait);
		b.AppendColumn("chain_wait_ns", wait_ns);
		b.AppendColumn("chain_trace_id", trace);
		b.AppendColumn("chain_span_id", span);
	}
};

struct PodColumns {
	std::shared_ptr<ch::ColumnString> uid = std::make_shared<ch::ColumnString>();
	std::shared_ptr<LcString> pod = std::make_shared<LcString>();
	std::shared_ptr<LcString> ns = std::make_shared<LcString>();
	std::shared_ptr<LcString> container = std::make_shared<LcString>();

	void append(const model::PodRef& p) {
		uid->Append(p.uid);
		pod->Append(p.name);
		ns->Append(p.ns);
		container->Append(p.container);
	}

	void add_to(ch::Block& b) const {
		b.AppendColumn("pod_uid", uid);
		b.AppendColumn("pod", pod);
		b.AppendColumn("namespace", ns);
		b.AppendColumn("container", container);
	}
};

// Where accumulates a WHERE clause with typed query parameters.
struct Where {
	std::vector<std::string> conds;
	ch::QueryParams params;

	std::string param(const std::string& type, std::string value) {
		auto name = std::format("p{}", params.size());
		params[name] = std::move(value);
		return std::format("{{{}:{}}}", name, type);
	}

	std::string sql() const {
		if (conds.empty()) {
			return "";
		}
		std::string out = "WHERE ";
		for (size_t i = 0; i < conds.size(); i++) {
			if (i > 0) {
				out += " AND ";
			}
			out += conds[i];
		}
		return out;
	}
};

Where build_where(const Filter& f, const std::string& start_col, const std::string& end_col) {
	Where w;
	if (!is_zero(f.from)) {
		w.conds.push_back(std::format("{} >= fromUnixTimestamp64Nano({}, 'UTC')", end_col,
		                              w.param("Int64", std::to_string(unix_nano(f.from)))));
	}
	if (!is_zero(f.to)) {
		w.conds.push_back(std::format("{} <= fromUnixTimestamp64Nano({}, 'UTC')", start_col,
		                              w.param("Int64", std::to_string(unix_nano(f.to)))));
	}
	auto eq = [&](const std::string& col, const std::string& v) {
		if (!v.empty()) {
			w.conds.push_back(std::format("{} = {}", col, w.param("String", v)));
		}
	};
	eq("node", f.node);
	eq("comm", f.comm);
	if (f.pid != 0) {
		w.conds.push_back(std::format("pid = {}", w.param("UInt32", std::to_string(f.pid))));
	}
	eq("container_id", f.container_id);
	eq("namespace", f.ns);
	eq("pod", f.pod);
	for (const auto& [k, v] : f.labels) {
		auto pk = w.param("String", k);
		w.conds.push_back(std::format("labels[{}] = {}", pk, w.param("String", v)));
	}
	return w;
}

int limit_of(int n) {
	return n <= 0 || n > 100000 ? 10000 : n;
}

constexpr const char* kSampleColumns =
        "batch_id, window_start, window_end, node, agent_id, labels, pid, tid, comm, container_id, reason, stack, "
        "chain_comm, chain_pid, chain_wake_stack, chain_wait_stack, chain_wait_ns, chain_trace_id, chain_span_id, "
        "blocked_ns, count, pod_uid, pod, namespace, container";
constexpr const char* kEventColumns =
        "batch_id, node, agent_id, labels, trace_id, span_id, start_time, end_time, duration_ns, pid, tid, comm, "
        "container_id, reason, stack, chain_comm, chain_pid, chain_wake_stack, chain_wait_stack, chain_wait_ns, "
        "chain_trace_id, chain_span_id, pod_uid, pod, namespace, container, gpu_stream, gpu_launches";

}  // namespace

// Lease returns its connection to the pool unless the query failed, in
// which case the connection may be mid-protocol and is dropped.
class Store::Lease {
public:
	Lease(Store& s, std::unique_ptr<ch::Client> c) : s_(s), c_(std::move(c)) {}
	Lease(Lease&& o) noexcept : s_(o.s_), c_(std::move(o.c_)), ok_(o.ok_), live_(o.live_) { o.live_ = false; }
	Lease(const Lease&) = delete;
	Lease& operator=(const Lease&) = delete;
	Lease& operator=(Lease&&) = delete;
	~Lease() {
		if (!live_) {
			return;
		}
		std::unique_ptr<ch::Client> back;
		if (ok_) {
			back = std::move(c_);
		}
		c_.reset();
		s_.give_back(std::move(back));
	}
	ch::Client* operator->() { return c_.get(); }
	void done() { ok_ = true; }

private:
	Store& s_;
	std::unique_ptr<ch::Client> c_;
	bool ok_ = false;
	bool live_ = true;
};

Store::Store(Options o) : opts_(std::move(o)) {
	auto db = quote_ident(opts_.database) + ".";
	samples_ = db + quote_ident(opts_.samples_table);
	events_ = db + quote_ident(opts_.events_table);
	if (opts_.auto_migrate) {
		migrate();
	}
}

Store::~Store() {
	close();
}

std::unique_ptr<ch::Client> Store::connect() {
	ch::ClientOptions co;
	std::vector<ch::Endpoint> eps;
	for (const auto& a : opts_.addrs) {
		eps.push_back(endpoint(a));
	}
	auto ms = [](Duration d) { return std::chrono::duration_cast<std::chrono::milliseconds>(d); };
	co.SetEndpoints(eps)
	        .SetUser(opts_.username.empty() ? "default" : opts_.username)
	        .SetPassword(opts_.password)
	        .SetCompressionMethod(ch::CompressionMethod::LZ4)
	        .SetConnectionConnectTimeout(ms(opts_.dial_timeout))
	        .SetConnectionRecvTimeout(ms(opts_.query_timeout))
	        .SetConnectionSendTimeout(ms(opts_.query_timeout))
	        .SetSendRetries(1);
	try {
		return std::make_unique<ch::Client>(co);
	} catch (const std::exception& e) {
		throw Error(std::format("open clickhouse: {}", e.what()));
	}
}

Store::Lease Store::acquire() {
	{
		std::unique_lock lock(mu_);
		if (!cv_.wait_for(lock, opts_.query_timeout,
		                  [this] { return closed_ || !idle_.empty() || open_ < opts_.max_open_conns; })) {
			throw Error("clickhouse: no free connection");
		}
		if (closed_) {
			throw Error("clickhouse: store closed");
		}
		if (!idle_.empty()) {
			auto c = std::move(idle_.back());
			idle_.pop_back();
			return Lease(*this, std::move(c));
		}
		open_++;
	}
	try {
		return Lease(*this, connect());
	} catch (...) {
		give_back(nullptr);
		throw;
	}
}

void Store::give_back(std::unique_ptr<ch::Client> c) {
	{
		std::lock_guard lock(mu_);
		if (c && !closed_) {
			idle_.push_back(std::move(c));
		} else {
			open_--;
		}
	}
	cv_.notify_one();
}

void Store::execute(const std::string& sql) {
	auto c = acquire();
	c->Execute(ch::Query(sql));
	c.done();
}

void Store::migrate() {
	for (const auto& stmt : schema_statements(opts_)) {
		try {
			execute(stmt);
		} catch (const std::exception& e) {
			throw Error(std::format("migrate: {}", e.what()));
		}
	}
}

void Store::write_batch(const model::Batch& b) {
	auto labels = b.resource.labels;
	auto insert = [&](const std::string& table, const char* columns, const std::string& token, ch::Block& block) {
		std::string sql = std::format(
		        "INSERT INTO {} ({}) SETTINGS insert_deduplicate = 1, insert_deduplication_token = {}", table,
		        columns, sql_string(token));
		if (!opts_.cluster.empty()) {
			sql += ", insert_distributed_sync = 1";
		}
		sql += " VALUES";
		auto c = acquire();
		try {
			c->BeginInsert(sql);
			c->SendInsertBlock(block);
			c->EndInsert();
		} catch (const std::exception& e) {
			throw Error(std::format("send {}: {}", table, e.what()));
		}
		c.done();
	};
	auto label_col = [&] {
		return std::make_shared<ch::ColumnMapT<LcString, ch::ColumnString>>(
		        std::make_shared<LcString>(), std::make_shared<ch::ColumnString>());
	};
	if (!b.offcpu.empty()) {
		ch::Block block;
		auto batch_id = std::make_shared<ch::ColumnString>();
		auto ws = std::make_shared<ch::ColumnDateTime64>(9, "UTC");
		auto we = std::make_shared<ch::ColumnDateTime64>(9, "UTC");
		auto node = std::make_shared<LcString>();
		auto agent = std::make_shared<ch::ColumnString>();
		auto lbl = label_col();
		auto pid = std::make_shared<ch::ColumnUInt32>();
		auto tid = std::make_shared<ch::ColumnUInt32>();
		auto comm = std::make_shared<LcString>();
		auto cid = std::make_shared<ch::ColumnString>();
		auto reason = std::make_shared<LcString>();
		auto stack = std::make_shared<ch::ColumnArrayT<ch::ColumnString>>();
		auto blocked = std::make_shared<ch::ColumnUInt64>();
		auto count = std::make_shared<ch::ColumnUInt64>();
		ChainColumns chain;
		PodColumns pod;
		for (const auto& s : b.offcpu) {
			batch_id->Append(b.id);
			ws->Append(unix_nano(b.window_start));
			we->Append(unix_nano(b.window_end));
			node->Append(b.resource.node);
			agent->Append(b.resource.agent_id);
			lbl->Append(labels);
			pid->Append(s.task.pid);
			tid->Append(s.task.tid);
			comm->Append(s.task.comm);
			cid->Append(s.task.container_id);
			reason->Append(model::to_string(s.reason));
			stack->Append(model::symbols(s.stack));
			chain.append(s.chain);
			blocked->Append(s.blocked_ns);
			count->Append(s.count);
			pod.append(s.task.pod);
		}
		block.AppendColumn("batch_id", batch_id);
		block.AppendColumn("window_start", ws);
		block.AppendColumn("window_end", we);
		block.AppendColumn("node", node);
		block.AppendColumn("agent_id", agent);
		block.AppendColumn("labels", lbl);
		block.AppendColumn("pid", pid);
		block.AppendColumn("tid", tid);
		block.AppendColumn("comm", comm);
		block.AppendColumn("container_id", cid);
		block.AppendColumn("reason", reason);
		block.AppendColumn("stack", stack);
		chain.add_to(block);
		block.AppendColumn("blocked_ns", blocked);
		block.AppendColumn("count", count);
		pod.add_to(block);
		insert(samples_, kSampleColumns, b.id + ":samples", block);
	}
	if (!b.events.empty()) {
		ch::Block block;
		auto batch_id = std::make_shared<ch::ColumnString>();
		auto node = std::make_shared<LcString>();
		auto agent = std::make_shared<ch::ColumnString>();
		auto lbl = label_col();
		auto trace = std::make_shared<ch::ColumnString>();
		auto span = std::make_shared<ch::ColumnString>();
		auto start = std::make_shared<ch::ColumnDateTime64>(9, "UTC");
		auto end = std::make_shared<ch::ColumnDateTime64>(9, "UTC");
		auto dur = std::make_shared<ch::ColumnUInt64>();
		auto pid = std::make_shared<ch::ColumnUInt32>();
		auto tid = std::make_shared<ch::ColumnUInt32>();
		auto comm = std::make_shared<LcString>();
		auto cid = std::make_shared<ch::ColumnString>();
		auto reason = std::make_shared<LcString>();
		auto stack = std::make_shared<ch::ColumnArrayT<ch::ColumnString>>();
		auto gpu_stream = std::make_shared<ch::ColumnUInt64>();
		auto gpu_launches = std::make_shared<ch::ColumnUInt32>();
		ChainColumns chain;
		PodColumns pod;
		for (const auto& e : b.events) {
			batch_id->Append(b.id);
			node->Append(b.resource.node);
			agent->Append(b.resource.agent_id);
			lbl->Append(labels);
			trace->Append(e.trace.trace_id_hex());
			span->Append(e.trace.span_id_hex());
			start->Append(unix_nano(e.start));
			end->Append(unix_nano(e.end));
			dur->Append(static_cast<uint64_t>(e.duration().count()));
			pid->Append(e.task.pid);
			tid->Append(e.task.tid);
			comm->Append(e.task.comm);
			cid->Append(e.task.container_id);
			reason->Append(model::to_string(e.reason));
			stack->Append(model::symbols(e.stack));
			chain.append(e.chain);
			pod.append(e.task.pod);
			gpu_stream->Append(e.gpu.stream);
			gpu_launches->Append(e.gpu.launches);
		}
		block.AppendColumn("batch_id", batch_id);
		block.AppendColumn("node", node);
		block.AppendColumn("agent_id", agent);
		block.AppendColumn("labels", lbl);
		block.AppendColumn("trace_id", trace);
		block.AppendColumn("span_id", span);
		block.AppendColumn("start_time", start);
		block.AppendColumn("end_time", end);
		block.AppendColumn("duration_ns", dur);
		block.AppendColumn("pid", pid);
		block.AppendColumn("tid", tid);
		block.AppendColumn("comm", comm);
		block.AppendColumn("container_id", cid);
		block.AppendColumn("reason", reason);
		block.AppendColumn("stack", stack);
		chain.add_to(block);
		pod.add_to(block);
		block.AppendColumn("gpu_stream", gpu_stream);
		block.AppendColumn("gpu_launches", gpu_launches);
		insert(events_, kEventColumns, b.id + ":events", block);
	}
}

std::vector<ChainValue> Store::offcpu_chains(const ChainQuery& q) {
	auto w = build_where(q.filter, "window_start", "window_end");
	int depth = q.depth < 0 || q.depth > 16 ? 16 : q.depth;
	auto sql = std::format(R"(SELECT toString(comm) AS c, stack,
	CAST(arraySlice(chain_comm, 1, {0}) AS Array(String)) AS cc,
	arraySlice(chain_wake_stack, 1, {0}) AS cwk,
	arraySlice(chain_wait_stack, 1, {0}) AS cwt,
	sumForEach(arraySlice(chain_wait_ns, 1, {0})) AS cwn,
	sum(blocked_ns), sum(count)
FROM {1} {2}
GROUP BY c, stack, cc, cwk, cwt
ORDER BY 7 DESC
LIMIT {3})",
	                       depth, samples_, w.sql(), limit_of(q.limit));
	std::vector<ChainValue> out;
	ch::Query query(sql);
	query.SetParams(w.params);
	query.OnData([&](const ch::Block& b) {
		for (size_t r = 0; r < b.GetRowCount(); r++) {
			ChainValue v;
			v.comm = string_at(b[0], r);
			v.stack = strings_at(b[1], r);
			auto comms = strings_at(b[2], r);
			auto wake = nested_at(b[3], r);
			auto wait = nested_at(b[4], r);
			auto ns = numbers_at<uint64_t>(b[5], r);
			v.total_ns = number_at<uint64_t>(b[6], r);
			v.count = number_at<uint64_t>(b[7], r);
			for (size_t i = 0; i < comms.size(); i++) {
				Hop h;
				h.comm = comms[i];
				h.wake_stack = i < wake.size() ? wake[i] : std::vector<std::string>{};
				h.wait_stack = i < wait.size() ? wait[i] : std::vector<std::string>{};
				h.wait_ns = i < ns.size() ? ns[i] : 0;
				v.chain.push_back(std::move(h));
			}
			out.push_back(std::move(v));
		}
	});
	auto c = acquire();
	try {
		c->Select(query);
	} catch (const std::exception& e) {
		throw Error(std::format("query chains: {}", e.what()));
	}
	c.done();
	return out;
}

std::vector<Event> Store::events(const EventQuery& q) {
	auto w = build_where(q.filter, "start_time", "end_time");
	if (!q.trace_id.empty()) {
		w.conds.push_back("trace_id = " + w.param("String", q.trace_id));
	}
	if (!q.span_id.empty()) {
		w.conds.push_back("span_id = " + w.param("String", q.span_id));
	}
	if (q.min_duration > Duration(0)) {
		w.conds.push_back("duration_ns >= " + w.param("UInt64", std::to_string(q.min_duration.count())));
	}
	auto sql = std::format(
	        R"(SELECT toString(node), pid, tid, toString(comm), container_id, toString(pod), toString(namespace),
	toString(container), stack, CAST(chain_comm AS Array(String)), chain_pid, chain_wake_stack, chain_wait_stack,
	chain_wait_ns, chain_trace_id, chain_span_id, toUnixTimestamp64Nano(start_time), toUnixTimestamp64Nano(end_time),
	toString(reason), trace_id, span_id, gpu_stream, gpu_launches
FROM {} {} ORDER BY start_time LIMIT {})",
	        events_, w.sql(), limit_of(q.limit));
	std::vector<Event> out;
	ch::Query query(sql);
	query.SetParams(w.params);
	query.OnData([&](const ch::Block& b) {
		for (size_t r = 0; r < b.GetRowCount(); r++) {
			Event e;
			e.node = string_at(b[0], r);
			e.pid = number_at<uint32_t>(b[1], r);
			e.tid = number_at<uint32_t>(b[2], r);
			e.comm = string_at(b[3], r);
			e.container_id = string_at(b[4], r);
			e.pod = string_at(b[5], r);
			e.ns = string_at(b[6], r);
			e.container = string_at(b[7], r);
			e.stack = strings_at(b[8], r);
			auto comms = strings_at(b[9], r);
			auto pids = numbers_at<uint32_t>(b[10], r);
			auto wake = nested_at(b[11], r);
			auto wait = nested_at(b[12], r);
			auto ns = numbers_at<uint64_t>(b[13], r);
			auto traces = strings_at(b[14], r);
			auto spans = strings_at(b[15], r);
			for (size_t i = 0; i < comms.size(); i++) {
				e.chain.push_back(Hop{comms[i], i < pids.size() ? pids[i] : 0,
				                      i < wake.size() ? wake[i] : std::vector<std::string>{},
				                      i < wait.size() ? wait[i] : std::vector<std::string>{},
				                      i < ns.size() ? ns[i] : 0, i < traces.size() ? traces[i] : "",
				                      i < spans.size() ? spans[i] : ""});
			}
			e.start = from_unix_nano(number_at<int64_t>(b[16], r));
			e.end = from_unix_nano(number_at<int64_t>(b[17], r));
			e.reason = string_at(b[18], r);
			e.trace_id = string_at(b[19], r);
			e.span_id = string_at(b[20], r);
			e.gpu_stream = number_at<uint64_t>(b[21], r);
			e.gpu_launches = number_at<uint32_t>(b[22], r);
			out.push_back(std::move(e));
		}
	});
	auto c = acquire();
	try {
		c->Select(query);
	} catch (const std::exception& e) {
		throw Error(std::format("query events: {}", e.what()));
	}
	c.done();
	return out;
}

void Store::ping(Duration) {
	auto c = acquire();
	c->Ping();
	c.done();
}

void Store::close() {
	std::vector<std::unique_ptr<ch::Client>> idle;
	{
		std::lock_guard lock(mu_);
		closed_ = true;
		idle.swap(idle_);
		open_ -= static_cast<int>(idle.size());
	}
	cv_.notify_all();
}

}  // namespace etrace::storage::clickhouse
