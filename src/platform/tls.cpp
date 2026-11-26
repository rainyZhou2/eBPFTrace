#include "platform/tls.h"

#include <cerrno>
#include <cstring>
#include <format>
#include <fstream>
#include <sstream>

#include "platform/error.h"

namespace etrace::tls {

namespace {

std::string read_pem(const std::string& path, std::string_view what) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		throw Error(std::format("tls: read {} {}: {}", what, path, std::strerror(errno)));
	}
	std::ostringstream ss;
	ss << in.rdbuf();
	auto s = ss.str();
	if (s.find("-----BEGIN") == std::string::npos) {
		throw Error(std::format("tls: no PEM data in {} {}", what, path));
	}
	return s;
}

}  // namespace

void decode_fields(config::Fields& f, Config& c) {
	f("enabled", c.enabled)("cert_file", c.cert_file)("key_file", c.key_file)("ca_file", c.ca_file)(
	        "server_name", c.server_name)("require_client_cert", c.require_client_cert)(
	        "insecure_skip_verify", c.insecure_skip_verify)("allow_insecure", c.allow_insecure);
}

void validate_server(const Config& c) {
	if (!c.enabled) {
		if (c.require_client_cert) {
			throw Error("tls: require_client_cert needs tls.enabled");
		}
		return;
	}
	if (c.cert_file.empty() || c.key_file.empty()) {
		throw Error("tls: cert_file and key_file are required");
	}
	if (c.require_client_cert && c.ca_file.empty()) {
		throw Error("tls: ca_file is required when require_client_cert is set");
	}
}

void validate_client(const Config& c, bool has_credentials) {
	if (c.cert_file.empty() != c.key_file.empty()) {
		throw Error("tls: cert_file and key_file must be set together");
	}
	if (!c.enabled) {
		if (has_credentials && !c.allow_insecure) {
			throw Error("tls: credentials require TLS or an explicit tls.allow_insecure");
		}
		return;
	}
}

Material server_material(const Config& c) {
	validate_server(c);
	Material m{read_pem(c.cert_file, "cert"), read_pem(c.key_file, "key"), ""};
	if (!c.ca_file.empty()) {
		m.ca = read_pem(c.ca_file, "ca");
	}
	return m;
}

Material client_material(const Config& c) {
	Material m;
	if (!c.ca_file.empty()) {
		m.ca = read_pem(c.ca_file, "ca");
	}
	if (!c.cert_file.empty() && !c.key_file.empty()) {
		m.cert = read_pem(c.cert_file, "client cert");
		m.key = read_pem(c.key_file, "client key");
	}
	return m;
}

}  // namespace etrace::tls
