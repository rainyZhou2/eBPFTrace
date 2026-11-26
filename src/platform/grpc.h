#pragma once

#include <grpcpp/grpcpp.h>

#include <memory>
#include <stop_token>
#include <string>
#include <vector>

#include "platform/config.h"
#include "platform/log.h"
#include "platform/time.h"
#include "platform/tls.h"

namespace etrace::grpcx {

struct ServerConfig {
	std::string listen = ":4317";
	int max_recv_msg_bytes = 16 << 20;
	uint32_t max_concurrent_streams = 1024;
	Duration max_connection_age = 5min;
	Duration graceful_timeout = 15s;
	tls::Config tls;

	void validate() const;
};

void decode_fields(config::Fields& f, ServerConfig& c);

struct ClientConfig {
	std::string target;
	std::string token;
	tls::Config tls;
	int max_send_msg_bytes = 16 << 20;
	Duration keepalive_time = 30s;

	void validate() const;
};

void decode_fields(config::Fields& f, ClientConfig& c);

// Server owns a gRPC server with the standard health service. Bounded
// connection age makes clients reconnect periodically and rebalance across
// replicas behind an L4 load balancer.
class Server {
public:
	Server(ServerConfig cfg, log::Logger log);
	~Server();

	void register_service(grpc::Service* s) { services_.push_back(s); }
	void set_serving(bool serving);
	std::string name() const { return "grpc"; }
	// run starts serving and blocks until stop; then it marks the health
	// service NOT_SERVING and drains calls for up to graceful_timeout.
	void run(std::stop_token st);
	int bound_port() const { return port_; }

private:
	ServerConfig cfg_;
	log::Logger log_;
	std::vector<grpc::Service*> services_;
	std::mutex mu_;
	std::unique_ptr<grpc::Server> srv_;
	bool serving_ = true;
	int port_ = 0;
};

// dial uses client-side round robin with health checking. With a dns:///
// target that resolves to every collector replica, unhealthy replicas are
// skipped and load is spread across the rest.
std::shared_ptr<grpc::Channel> dial(const ClientConfig& c);

}  // namespace etrace::grpcx
