#include "platform/http.h"

#include <atomic>
#include <condition_variable>
#include <format>
#include <mutex>
#include <thread>

#include "platform/error.h"

namespace etrace::http {

void decode_fields(config::Fields& f, ServerConfig& c) {
	f("listen", c.listen)("read_timeout", c.read_timeout)("write_timeout", c.write_timeout)(
	        "graceful_timeout", c.graceful_timeout)("tls", c.tls);
}

std::pair<std::string, int> split_listen(const std::string& listen) {
	auto colon = listen.rfind(':');
	if (colon == std::string::npos) {
		throw Error(std::format("listen address \"{}\" has no port", listen));
	}
	std::string host = listen.substr(0, colon);
	if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
		host = host.substr(1, host.size() - 2);
	}
	int port;
	try {
		size_t used;
		port = std::stoi(listen.substr(colon + 1), &used);
		if (used != listen.size() - colon - 1 || port < 0 || port > 65535) {
			throw std::invalid_argument("port");
		}
	} catch (const std::exception&) {
		throw Error(std::format("listen address \"{}\" has an invalid port", listen));
	}
	if (host.empty()) {
		host = "0.0.0.0";
	}
	return {host, port};
}

Server::Server(std::string name, ServerConfig cfg, log::Logger log)
    : name_(std::move(name)), cfg_(std::move(cfg)), log_(std::move(log)) {
	tls::validate_server(cfg_.tls);
	if (cfg_.tls.enabled) {
		tls::server_material(cfg_.tls);
		const char* ca = cfg_.tls.require_client_cert ? cfg_.tls.ca_file.c_str() : nullptr;
		auto ssl =
		        std::make_unique<httplib::SSLServer>(cfg_.tls.cert_file.c_str(), cfg_.tls.key_file.c_str(), ca);
		if (!ssl->is_valid()) {
			throw Error(std::format("{}: tls: cannot load certificate or key", name_));
		}
		srv_ = std::move(ssl);
	} else {
		srv_ = std::make_unique<httplib::Server>();
	}
	auto secs = [](Duration d) { return std::chrono::duration_cast<std::chrono::microseconds>(d); };
	srv_->set_read_timeout(secs(cfg_.read_timeout));
	srv_->set_write_timeout(secs(cfg_.write_timeout));
	srv_->set_keep_alive_timeout(5);
	srv_->set_payload_max_length(1 << 20);
}

void Server::run(std::stop_token st) {
	auto [host, port] = split_listen(cfg_.listen);
	if (port == 0) {
		port_ = srv_->bind_to_any_port(host);
		if (port_ < 0) {
			throw Error(std::format("{}: listen {}: bind failed", name_, cfg_.listen));
		}
	} else if (!srv_->bind_to_port(host, port)) {
		throw Error(std::format("{}: listen {}: bind failed", name_, cfg_.listen));
	} else {
		port_ = port;
	}
	log_.info("http server listening", {{"server", name_}, {"addr", std::format("{}:{}", host, port_)}});
	// httplib ignores stop() until the accept loop runs, so the stopper
	// waits for readiness before stopping.
	std::atomic<bool> finished{false};
	std::stop_source local;
	std::stop_callback on_stop(st, [&] { local.request_stop(); });
	std::jthread stopper([this, tok = local.get_token(), &finished] {
		std::mutex mu;
		std::condition_variable_any cv;
		std::unique_lock lock(mu);
		cv.wait(lock, tok, [] { return false; });
		if (finished.load()) {
			return;
		}
		srv_->wait_until_ready();
		srv_->stop();
	});
	bool ok = srv_->listen_after_bind();
	finished.store(true);
	local.request_stop();
	stopper.join();
	if (!ok && !st.stop_requested()) {
		throw Error(std::format("{}: serve failed", name_));
	}
}

void write_json(httplib::Response& res, int status, const std::string& body) {
	res.status = status;
	res.set_content(body + "\n", "application/json");
}

}  // namespace etrace::http
