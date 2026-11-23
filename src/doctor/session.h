#pragma once

#include <functional>
#include <stop_token>
#include <vector>

#include "agent/plugin.h"
#include "model/model.h"
#include "platform/log.h"
#include "platform/time.h"
#include "storage/storage.h"

namespace etrace::doctor {

struct Window {
	Time start{};
	Time end{};
	int batches = 0;
	// blocked holds the threads still off-CPU when recording stopped,
	// symbolized like collected data.
	std::vector<model::OffCPUEvent> blocked;
	// interrupted is set when the caller stopped before the duration
	// elapsed; the data collected so far is still kept.
	bool interrupted = false;
};

// Session drains a started source every interval, symbolizes each batch
// right away so processes that exit later still resolve, and stores it.
struct Session {
	agent::Source& source;
	agent::ProcessorChain& processors;
	storage::Writer& store;
	model::Resource resource;
	Duration interval = 5s;
	// skip_pid drops the recorder's own threads, which would otherwise top
	// the report with its own waits.
	uint32_t skip_pid = 0;
	// blocked lists threads still off-CPU, whose waits have not ended and so
	// are not collected yet; unset when the source cannot.
	std::function<std::vector<model::OffCPUEvent>()> blocked;
	log::Logger log = log::Logger::discard();
	std::function<Time()> now = etrace::now;

	Window run(std::stop_token st, Duration d);

private:
	void collect(Window& w, Time from);
	void list_blocked(Window& w);
	void skip_self(std::vector<model::OffCPUEvent>& evs) const;
};

}  // namespace etrace::doctor
