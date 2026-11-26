#pragma once

#include <memory>
#include <string>
#include <vector>

#include "query/flame.h"
#include "query/json.h"
#include "query/pprof.h"
#include "storage/storage.h"
#include "tracing/tracing.h"

namespace etrace::query {

inline constexpr std::string_view kWakeSeparator = "--";
inline constexpr std::string_view kHopPrefix = "<- ";
inline constexpr std::string_view kWaitPrefix = "waiting: ";

struct FlameRequest {
	storage::Filter filter;
	int depth = 0;
	int limit = 0;
};

enum class CauseEvidence { blocked_site, observed_wakeup, inferred_upstream };

struct CausePath {
	std::vector<std::string> path;
	std::string blocked;
	std::string root;
	uint64_t total_ns = 0;
	uint64_t count = 0;
	std::vector<uint64_t> hops_wait;
	CauseEvidence evidence = CauseEvidence::blocked_site;
	uint32_t observed_hops = 0;
	uint32_t inferred_hops = 0;
	uint32_t inferred_waits = 0;
};

Json to_json(const CausePath& c);
Json to_json(const std::vector<CausePath>& cs);

struct SpanOffCPU {
	tracing::Span span;
	uint64_t duration_ns = 0;
	uint64_t offcpu_ns = 0;
	uint64_t self_offcpu_ns = 0;
	std::vector<storage::Event> events;
};

struct TraceReport {
	std::string trace_id;
	std::vector<SpanOffCPU> spans;
	std::vector<storage::Event> unattributed;
	uint64_t offcpu_total_ns = 0;
	std::vector<CausePath> causes;
	std::string partial;
};

Json to_json(const TraceReport& r);

// Service turns stored off-CPU data into flame graphs, causes and
// per-trace reports. It is shared by the query API, the Pyroscope exporter
// and the doctor report.
class Service {
public:
	explicit Service(storage::Reader& r) : reader_(r) {}

	// lines renders off-CPU stacks root-first. Each hop of the causal chain
	// is stacked above the previous one: a separator, the hop's task, the
	// stack it used to wake the previous task, and, when the hop itself was
	// blocked during that wait, the stack it was blocked in.
	std::vector<flame::Line> lines(const FlameRequest& req);
	std::unique_ptr<flame::Node> tree(const FlameRequest& req);
	pprof::Profile profile(const FlameRequest& req);
	std::vector<CausePath> causes(const storage::Filter& f, int depth, int limit);
	// trace_report attributes every off-CPU interval recorded under a trace
	// id to the span that was active on the thread, and rolls the time up to
	// ancestors. The limit applies only to event details. Without a backend,
	// or when it fails, event details are unattributed and partial explains why.
	TraceReport trace_report(tracing::Backend* backend, const std::string& trace_id, const storage::Filter& f,
	                         int limit);
	storage::Reader& reader() { return reader_; }

private:
	storage::Reader& reader_;
};

std::vector<std::string> chain_path(const storage::ChainValue& v);
// offcpu_profile builds a pprof profile with wait time and wait counts
// per flame path. pprof stacks are leaf first, so the deepest hop of the
// causal chain becomes the leaf. sample_type names the time sample.
pprof::Profile offcpu_profile(const std::vector<storage::ChainValue>& vals, Time from, Time to,
                              const std::string& sample_type = "off_cpu");
std::vector<CausePath> collapse_causes(const std::vector<storage::ChainValue>& vals, int limit);
// blocked_frame names a stack by its innermost frame that is not part of
// the scheduler's own sleep or wakeup plumbing.
std::string blocked_frame(const std::vector<std::string>& stack);

}  // namespace etrace::query
