#pragma once

#include <httplib.h>

#include <memory>
#include <stop_token>
#include <string>
#include <utility>

#include "platform/config.h"
#include "platform/log.h"
#include "platform/time.h"
#include "platform/tls.h"

namespace etrace::http {

struct ServerConfig {
	std::string listen = ":8080";
	Duration read_timeout = 10s;
	Duration write_timeout = 60s;
	Duration graceful_timeout = 15s;
	tls::Config tls;
};

void decode_fields(config::Fields& f, ServerConfig& c);

// split_listen turns "host:port", ":port" or "[v6]:port" into a bind host
// and port; an empty host binds every interface.
std::pair<std::string, int> split_listen(const std::string& listen);

// Server runs an httplib server until stopped. Routes are installed by the
// caller on server() before run.
class Server {
public:
	Server(std::string name, ServerConfig cfg, log::Logger log);

	httplib::Server& server() { return *srv_; }
	std::string name() const { return name_; }
	// run binds, serves, and on stop stops accepting and waits for workers
	// to finish their requests.
	void run(std::stop_token st);
	int bound_port() const { return port_; }

private:
	std::string name_;
	ServerConfig cfg_;
	log::Logger log_;
	std::unique_ptr<httplib::Server> srv_;
	int port_ = 0;
};

void write_json(httplib::Response& res, int status, const std::string& body);

}  // namespace etrace::http
