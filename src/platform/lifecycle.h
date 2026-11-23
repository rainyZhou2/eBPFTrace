#pragma once

#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "platform/time.h"

namespace etrace::lifecycle {

// Component is a long-running part of a service. run returns when stop is
// requested or when it fails; a thrown exception stops the whole service.
class Component {
public:
	virtual ~Component() = default;
	virtual std::string name() const = 0;
	virtual void run(std::stop_token st) = 0;
};

class FuncComponent : public Component {
public:
	FuncComponent(std::string name, std::function<void(std::stop_token)> fn)
	    : name_(std::move(name)), fn_(std::move(fn)) {}
	std::string name() const override { return name_; }
	void run(std::stop_token st) override { fn_(st); }

private:
	std::string name_;
	std::function<void(std::stop_token)> fn_;
};

using Closer = std::function<void()>;

// run starts every component on its own thread and stops all of them when
// st is stopped or any component returns. Closers run in reverse order once
// every component has returned. The first component failure, or else the
// first close failure, is rethrown as an Error naming its source. If
// shutting down takes longer than shutdown_timeout after the stop, the
// process exits with status 1 rather than hang.
void run(std::stop_token st, Duration shutdown_timeout, const std::vector<Component*>& components,
         const std::vector<Closer>& closers = {});

// SignalWatcher turns SIGINT and SIGTERM into a stop request. It must be
// created before any other thread so that every thread inherits the blocked
// signal mask.
class SignalWatcher {
public:
	SignalWatcher();
	~SignalWatcher();
	SignalWatcher(const SignalWatcher&) = delete;
	SignalWatcher& operator=(const SignalWatcher&) = delete;

	std::stop_token token() const { return source_.get_token(); }

private:
	std::stop_source source_;
	std::jthread waiter_;
};

}  // namespace etrace::lifecycle
