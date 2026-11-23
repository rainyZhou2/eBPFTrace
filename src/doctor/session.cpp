#include "doctor/session.h"

#include <algorithm>
#include <condition_variable>
#include <format>
#include <mutex>

namespace etrace::doctor {

Window Session::run(std::stop_token st, Duration d) {
	Window w;
	w.start = now();
	auto deadline = std::chrono::steady_clock::now() + d;
	auto next = std::chrono::steady_clock::now() + interval;
	auto last = w.start;
	std::mutex mu;
	std::condition_variable_any cv;
	while (true) {
		auto wake = std::min(next, deadline);
		{
			std::unique_lock lock(mu);
			cv.wait_until(lock, st, wake, [] { return false; });
		}
		if (st.stop_requested()) {
			w.interrupted = true;
			break;
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			break;
		}
		collect(w, last);
		last = now();
		next += interval;
	}
	collect(w, last);
	w.end = now();
	list_blocked(w);
	return w;
}

void Session::skip_self(std::vector<model::OffCPUEvent>& evs) const {
	if (skip_pid == 0) {
		return;
	}
	std::erase_if(evs, [this](const auto& e) { return e.task.pid == skip_pid; });
}

void Session::list_blocked(Window& w) {
	if (!blocked) {
		return;
	}
	std::vector<model::OffCPUEvent> evs;
	try {
		evs = blocked();
	} catch (const std::exception& e) {
		throw Error(std::format("list blocked threads: {}", e.what()));
	}
	model::Batch b;
	b.events = std::move(evs);
	skip_self(b.events);
	if (b.events.empty()) {
		return;
	}
	try {
		processors.process(b);
	} catch (const std::exception& e) {
		log.warn("processing failed, keeping unsymbolized stacks", {{"err", e}});
	}
	w.blocked = std::move(b.events);
}

void Session::collect(Window& w, Time from) {
	auto to = now();
	model::Batch b;
	b.id = std::format("doctor-{}", w.batches);
	b.resource = resource;
	b.window_start = from;
	b.window_end = to;
	try {
		source.collect(b);
	} catch (const std::exception& e) {
		throw Error(std::format("collect: {}", e.what()));
	}
	if (skip_pid != 0) {
		std::erase_if(b.offcpu, [this](const auto& s) { return s.task.pid == skip_pid; });
		skip_self(b.events);
	}
	if (b.empty()) {
		return;
	}
	try {
		processors.process(b);
	} catch (const std::exception& e) {
		log.warn("processing failed, keeping unsymbolized stacks", {{"err", e}});
	}
	try {
		store.write_batch(b);
	} catch (const storage::DuplicateError&) {
	} catch (const std::exception& e) {
		throw Error(std::format("store batch: {}", e.what()));
	}
	w.batches++;
}

}  // namespace etrace::doctor
