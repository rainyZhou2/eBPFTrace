#include "platform/lifecycle.h"

#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <condition_variable>
#include <cstdio>
#include <exception>
#include <format>
#include <mutex>
#include <optional>
#include <thread>

#include "platform/error.h"

namespace etrace::lifecycle {

namespace {

std::string describe(std::exception_ptr ep) {
	try {
		std::rethrow_exception(ep);
	} catch (const std::exception& e) {
		return e.what();
	} catch (...) {
		return "unknown error";
	}
}

}  // namespace

void run(std::stop_token st, Duration shutdown_timeout, const std::vector<Component*>& components,
         const std::vector<Closer>& closers) {
	std::stop_source stop;
	std::mutex mu;
	std::condition_variable done_cv;
	bool done = false;
	std::optional<std::string> first_err;

	// The watchdog bounds shutdown: once a stop is requested, components and
	// closers get shutdown_timeout to finish.
	std::jthread watchdog([&](std::stop_token) {
		std::unique_lock lock(mu);
		done_cv.wait(lock, [&] { return done || stop.stop_requested(); });
		if (done) {
			return;
		}
		if (!done_cv.wait_for(lock, shutdown_timeout, [&] { return done; })) {
			std::fprintf(stderr, "shutdown did not finish within %s, exiting\n",
			             format_duration(shutdown_timeout).c_str());
			std::fflush(stderr);
			std::_Exit(1);
		}
	});
	std::stop_callback on_parent(st, [&] {
		stop.request_stop();
		done_cv.notify_all();
	});

	{
		std::vector<std::jthread> threads;
		threads.reserve(components.size());
		for (auto* c : components) {
			threads.emplace_back([&, c] {
				try {
					c->run(stop.get_token());
				} catch (...) {
					auto msg = std::format("{}: {}", c->name(), describe(std::current_exception()));
					std::lock_guard lock(mu);
					if (!first_err) {
						first_err = std::move(msg);
					}
				}
				stop.request_stop();
				done_cv.notify_all();
			});
		}
	}

	std::optional<std::string> close_err;
	for (auto it = closers.rbegin(); it != closers.rend(); ++it) {
		try {
			(*it)();
		} catch (...) {
			if (!close_err) {
				close_err = describe(std::current_exception());
			}
		}
	}
	{
		std::lock_guard lock(mu);
		done = true;
	}
	done_cv.notify_all();
	if (first_err) {
		throw Error(*first_err);
	}
	if (close_err) {
		throw Error(*close_err);
	}
}

SignalWatcher::SignalWatcher() {
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGTERM);
	pthread_sigmask(SIG_BLOCK, &set, nullptr);
	waiter_ = std::jthread([this, set](std::stop_token st) {
		timespec poll{0, 200 * 1000 * 1000};
		while (!st.stop_requested()) {
			int sig = sigtimedwait(&set, nullptr, &poll);
			if (sig == SIGINT || sig == SIGTERM) {
				source_.request_stop();
			}
		}
	});
}

SignalWatcher::~SignalWatcher() {
	waiter_.request_stop();
	waiter_.join();
}

}  // namespace etrace::lifecycle
