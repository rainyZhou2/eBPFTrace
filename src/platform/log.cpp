#include "platform/log.h"

#include <cctype>
#include <format>
#include <iostream>

#include "platform/error.h"

namespace etrace::log {

namespace {

const char* level_name(Level l) {
	switch (l) {
		case Level::debug:
			return "DEBUG";
		case Level::info:
			return "INFO";
		case Level::warn:
			return "WARN";
		case Level::error:
			return "ERROR";
	}
	return "INFO";
}

void json_string(std::string& out, std::string_view s) {
	out += '"';
	for (unsigned char c : s) {
		switch (c) {
			case '"':
				out += "\\\"";
				break;
			case '\\':
				out += "\\\\";
				break;
			case '\n':
				out += "\\n";
				break;
			case '\r':
				out += "\\r";
				break;
			case '\t':
				out += "\\t";
				break;
			default:
				if (c < 0x20) {
					out += std::format("\\u{:04x}", c);
				} else {
					out += static_cast<char>(c);
				}
		}
	}
	out += '"';
}

bool needs_quote(std::string_view s) {
	if (s.empty()) {
		return true;
	}
	for (unsigned char c : s) {
		if (c <= ' ' || c == '"' || c == '=' || c == '\\' || c == 0x7f) {
			return true;
		}
	}
	return false;
}

void text_value(std::string& out, std::string_view s) {
	if (needs_quote(s)) {
		json_string(out, s);
	} else {
		out += s;
	}
}

}  // namespace

void Sink::write(Level l, std::string_view msg, const std::vector<Attr>& base, Attrs attrs) {
	std::string line;
	line.reserve(128);
	auto ts = format_rfc3339nano(now());
	if (format_ == Format::json) {
		line += "{\"time\":";
		json_string(line, ts);
		line += ",\"level\":";
		json_string(line, level_name(l));
		line += ",\"msg\":";
		json_string(line, msg);
		auto add = [&](const Attr& a) {
			line += ',';
			json_string(line, a.key);
			line += ':';
			if (a.quoted) {
				json_string(line, a.value);
			} else {
				line += a.value;
			}
		};
		for (const auto& a : base) {
			add(a);
		}
		for (const auto& a : attrs) {
			add(a);
		}
		line += "}\n";
	} else {
		line += "time=";
		line += ts;
		line += " level=";
		line += level_name(l);
		line += " msg=";
		text_value(line, msg);
		auto add = [&](const Attr& a) {
			line += ' ';
			line += a.key;
			line += '=';
			text_value(line, a.value);
		};
		for (const auto& a : base) {
			add(a);
		}
		for (const auto& a : attrs) {
			add(a);
		}
		line += '\n';
	}
	std::lock_guard lock(mu_);
	out_ << line;
	out_.flush();
}

Logger::Logger(std::shared_ptr<Sink> sink, Level level, std::vector<Attr> base)
    : sink_(std::move(sink)), level_(level), base_(std::move(base)) {}

Logger Logger::discard() {
	return Logger(nullptr, Level::error);
}

Logger Logger::with(Attrs attrs) const {
	auto base = base_;
	base.insert(base.end(), attrs.begin(), attrs.end());
	return Logger(sink_, level_, std::move(base));
}

void Logger::write(Level l, std::string_view msg, Attrs attrs) const {
	if (!enabled(l)) {
		return;
	}
	sink_->write(l, msg, base_, attrs);
}

Level parse_level(std::string_view s) {
	std::string lower;
	for (char c : s) {
		lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	if (lower == "debug") {
		return Level::debug;
	}
	if (lower == "info" || lower.empty()) {
		return Level::info;
	}
	if (lower == "warn" || lower == "warning") {
		return Level::warn;
	}
	if (lower == "error") {
		return Level::error;
	}
	throw Error(std::format("log level: unknown name \"{}\"", s));
}

Logger make(const Config& cfg, std::string_view service) {
	Level level = parse_level(cfg.level);
	Format format;
	if (cfg.format.empty() || cfg.format == "json") {
		format = Format::json;
	} else if (cfg.format == "text") {
		format = Format::text;
	} else {
		throw Error(std::format("unknown log format \"{}\"", cfg.format));
	}
	return Logger(std::make_shared<Sink>(std::cerr, format), level, {Attr("service", service)});
}

Logger make(std::ostream& out, Format format, Level level) {
	return Logger(std::make_shared<Sink>(out, format), level);
}

}  // namespace etrace::log
