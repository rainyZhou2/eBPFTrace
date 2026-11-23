#pragma once

#include <string>

#include "platform/config.h"

namespace etrace::tls {

struct Config {
	bool enabled = false;
	std::string cert_file;
	std::string key_file;
	std::string ca_file;
	std::string server_name;
	bool require_client_cert = false;
	bool insecure_skip_verify = false;
};

void decode_fields(config::Fields& f, Config& c);

// PEM material loaded from the configured files. Loading throws Error with
// the failing file so misconfiguration stops startup.
struct Material {
	std::string cert;
	std::string key;
	std::string ca;
};

// server_material requires a certificate and key.
Material server_material(const Config& c);
// client_material loads the optional CA and client key pair.
Material client_material(const Config& c);

}  // namespace etrace::tls
