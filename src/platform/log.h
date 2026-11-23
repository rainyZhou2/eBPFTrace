#pragma once

#include <chrono>
#include <concepts>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "platform/time.h"

namespace etrace::log {

enum class Level { debug = -4, info = 0, warn = 4, error = 8 };

enum class Format { json, text };

struct Config {
	std::string level = "info";
	std::string format = "json";
};

// Attr is one key/value pair of a structured log record. Numbers and
// booleans stay unquoted in JSON output.
struct Attr {
	std::string key;
	std::string value;
	bool quoted = true;

	Attr(std::string k, std::string_view v) : key(std::move(k)), value(v) {}
	Attr(std::string k, const char* v) : key(std::move(k)), value(v) {}
	Attr(std::string k, const std::string& v) : key(std::move(k)), value(v) {}
	Attr(std::string k, bool v) : key(std::move(k)), value(v ? "true" : "false"), quoted(false) {}
	template <std::integral T>
	Attr(std::string k, T v) : key(std::move(k)), value(std::to_string(v)), quoted(false) {}
	template <std::floating_point T>
	Attr(std::string k, T v) : key(std::move(k)), value(std::to_string(v)), quoted(false) {}
	Attr(std::string k, Duration d) : key(std::move(k)), value(format_duration(d)) {}
	Attr(std::string k, const std::exception& e) : key(std::move(k)), value(e.what()) {}
};

using Attrs = std::initializer_list<Attr>;

class Sink;

// Logger writes leveled, structured records. Copies share the sink and are
// safe to use from any thread.
class Logger {
public:
	// Discard drops every record; tests and optional components use it.
	static Logger discard();
	Logger(std::shared_ptr<Sink> sink, Level level, std::vector<Attr> base = {});

	Logger with(Attrs attrs) const;
	bool enabled(Level l) const { return sink_ && l >= level_; }

	void debug(std::string_view msg, Attrs attrs = {}) const { write(Level::debug, msg, attrs); }
	void info(std::string_view msg, Attrs attrs = {}) const { write(Level::info, msg, attrs); }
	void warn(std::string_view msg, Attrs attrs = {}) const { write(Level::warn, msg, attrs); }
	void error(std::string_view msg, Attrs attrs = {}) const { write(Level::error, msg, attrs); }

private:
	void write(Level l, std::string_view msg, Attrs attrs) const;

	std::shared_ptr<Sink> sink_;
	Level level_;
	std::vector<Attr> base_;
};

class Sink {
public:
	Sink(std::ostream& out, Format format) : out_(out), format_(format) {}
	void write(Level l, std::string_view msg, const std::vector<Attr>& base, Attrs attrs);

private:
	std::mutex mu_;
	std::ostream& out_;
	Format format_;
};

Level parse_level(std::string_view s);
// make builds a logger writing to stderr; it throws Error on an unknown
// level or format.
Logger make(const Config& cfg, std::string_view service);
Logger make(std::ostream& out, Format format, Level level);

}  // namespace etrace::log
