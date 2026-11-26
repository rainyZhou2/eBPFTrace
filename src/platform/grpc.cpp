#include "platform/grpc.h"

#include <grpcpp/ext/health_check_service_server_builder_option.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/security/tls_certificate_provider.h>
#include <grpcpp/security/tls_credentials_options.h>

#include <condition_variable>
#include <format>

#include "platform/error.h"

namespace etrace::grpcx {

namespace {

int ms(Duration d) {
	return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(d).count());
}

std::string grpc_listen_addr(const std::string& listen) {
	if (!listen.empty() && listen.front() == ':') {
		return "[::]" + listen;
	}
	return listen;
}

}  // namespace

void decode_fields(config::Fields& f, ServerConfig& c) {
	f("listen", c.listen)("max_recv_msg_bytes", c.max_recv_msg_bytes)(
	        "max_concurrent_streams", c.max_concurrent_streams)("max_connection_age", c.max_connection_age)(
	        "graceful_timeout", c.graceful_timeout)("tls", c.tls);
}

void decode_fields(config::Fields& f, ClientConfig& c) {
	f("target", c.target)("token", c.token)("tls", c.tls)("max_send_msg_bytes", c.max_send_msg_bytes)(
	        "keepalive_time", c.keepalive_time);
}

void ServerConfig::validate() const {
	if (max_recv_msg_bytes <= 0) {
		throw Error("grpc: max_recv_msg_bytes must be positive");
	}
	if (max_concurrent_streams == 0) {
		throw Error("grpc: max_concurrent_streams must be positive");
	}
	if (max_connection_age <= Duration(0) || graceful_timeout <= Duration(0)) {
		throw Error("grpc: connection age and graceful timeout must be positive");
	}
	::etrace::tls::validate_server(tls);
}

void ClientConfig::validate() const {
	if (target.empty()) {
		throw Error("grpc: target is required");
	}
	if (max_send_msg_bytes <= 0) {
		throw Error("grpc: max_send_msg_bytes must be positive");
	}
	if (keepalive_time <= Duration(0)) {
		throw Error("grpc: keepalive_time must be positive");
	}
	::etrace::tls::validate_client(tls, !token.empty());
}

Server::Server(ServerConfig cfg, log::Logger log) : cfg_(std::move(cfg)), log_(std::move(log)) {
	cfg_.validate();
}

Server::~Server() = default;

void Server::set_serving(bool serving) {
	std::lock_guard lock(mu_);
	serving_ = serving;
	if (srv_) {
		if (auto* h = srv_->GetHealthCheckService()) {
			h->SetServingStatus(serving);
		}
	}
}

void Server::run(std::stop_token st) {
	grpc::EnableDefaultHealthCheckService(true);
	grpc::ServerBuilder b;
	std::shared_ptr<grpc::ServerCredentials> creds;
	if (cfg_.tls.enabled) {
		auto m = tls::server_material(cfg_.tls);
		grpc::SslServerCredentialsOptions opts(
		        cfg_.tls.require_client_cert ? GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY
		                                     : GRPC_SSL_DONT_REQUEST_CLIENT_CERTIFICATE);
		opts.pem_root_certs = m.ca;
		opts.pem_key_cert_pairs.push_back({m.key, m.cert});
		creds = grpc::SslServerCredentials(opts);
	} else {
		creds = grpc::InsecureServerCredentials();
	}
	b.AddListeningPort(grpc_listen_addr(cfg_.listen), creds, &port_);
	b.SetMaxReceiveMessageSize(cfg_.max_recv_msg_bytes);
	b.AddChannelArgument(GRPC_ARG_MAX_CONCURRENT_STREAMS, static_cast<int>(cfg_.max_concurrent_streams));
	b.AddChannelArgument(GRPC_ARG_MAX_CONNECTION_AGE_MS, ms(cfg_.max_connection_age));
	b.AddChannelArgument(GRPC_ARG_MAX_CONNECTION_AGE_GRACE_MS, ms(cfg_.graceful_timeout));
	b.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS, 30000);
	b.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 10000);
	b.AddChannelArgument(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, 10000);
	b.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
	for (auto* s : services_) {
		b.RegisterService(s);
	}
	{
		std::lock_guard lock(mu_);
		srv_ = b.BuildAndStart();
		if (!srv_ || port_ == 0) {
			srv_.reset();
			throw Error(std::format("grpc: listen {} failed", cfg_.listen));
		}
		if (auto* h = srv_->GetHealthCheckService()) {
			h->SetServingStatus(serving_);
		}
	}
	log_.info("grpc server listening", {{"addr", cfg_.listen}, {"port", port_}});

	std::mutex mu;
	std::condition_variable_any cv;
	{
		std::unique_lock lock(mu);
		cv.wait(lock, st, [] { return false; });
	}
	grpc::Server* srv;
	{
		std::lock_guard lock(mu_);
		srv = srv_.get();
		if (auto* h = srv->GetHealthCheckService()) {
			h->Shutdown();
		}
	}
	srv->Shutdown(std::chrono::system_clock::now() + cfg_.graceful_timeout);
	srv->Wait();
}

std::shared_ptr<grpc::Channel> dial(const ClientConfig& c) {
	c.validate();
	grpc::ChannelArguments args;
	args.SetServiceConfigJSON(
	        R"({"loadBalancingConfig":[{"round_robin":{}}],"healthCheckConfig":{"serviceName":""}})");
	args.SetMaxSendMessageSize(c.max_send_msg_bytes);
	args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, ms(c.keepalive_time));
	args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 10000);
	args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
	std::shared_ptr<grpc::ChannelCredentials> creds;
	if (!c.tls.enabled) {
		creds = grpc::InsecureChannelCredentials();
	} else {
		auto m = tls::client_material(c.tls);
		if (!c.tls.server_name.empty()) {
			args.SetSslTargetNameOverride(c.tls.server_name);
		}
		if (c.tls.insecure_skip_verify) {
			// Without verification the CA is unused; only a client
			// certificate, when configured, is presented.
			grpc::experimental::TlsChannelCredentialsOptions o;
			if (!m.cert.empty()) {
				auto p = std::make_shared<grpc::experimental::InMemoryCertificateProvider>();
				std::vector<grpc::experimental::IdentityKeyOrSignerCertPair> pairs{{m.key, m.cert}};
				if (auto st = p->UpdateIdentityKeyCertPair(pairs); !st.ok()) {
					throw Error(std::format("tls: client key pair: {}", st.ToString()));
				}
				o.set_identity_certificate_provider(p);
			}
			o.set_verify_server_certs(false);
			creds = grpc::experimental::TlsCredentials(o);
		} else {
			grpc::SslCredentialsOptions o;
			o.pem_root_certs = m.ca;
			o.pem_private_key = m.key;
			o.pem_cert_chain = m.cert;
			creds = grpc::SslCredentials(o);
		}
	}
	return grpc::CreateCustomChannel(c.target, creds, args);
}

}  // namespace etrace::grpcx
