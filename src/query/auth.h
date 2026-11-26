#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "platform/auth.h"
#include "platform/config.h"

namespace etrace::query {

struct AuthConfig {
	std::string realm = "etrace";
	std::vector<std::string> tokens;
	std::string basic_user;
	std::string basic_password;
	bool allow_unauthenticated = false;
	bool allow_mtls = false;

	void check() const;
};

void decode_fields(config::Fields& f, AuthConfig& c);

class Auth {
public:
	explicit Auth(AuthConfig config);

	bool authorize(std::string_view authorization) const;
	std::string challenge() const;
	bool anonymous() const { return cfg_.allow_unauthenticated || cfg_.allow_mtls; }

private:
	AuthConfig cfg_;
	auth::TokenSet tokens_;
	bool basic_enabled_ = false;
};

}  // namespace etrace::query
