#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "platform/auth.h"

namespace etrace::collector {

struct AuthConfig {
	std::vector<std::string> tokens;
	bool allow_unauthenticated = false;
	bool allow_mtls = false;

	void validate() const;
};

class Auth {
public:
	explicit Auth(AuthConfig config);
	Auth(std::vector<std::string> tokens, bool allow_unauthenticated, bool allow_mtls);

	void validate() const;
	bool authorize(std::string_view authorization) const;
	bool anonymous() const { return cfg_.allow_unauthenticated || cfg_.allow_mtls; }

private:
	AuthConfig cfg_;
	auth::TokenSet tokens_;
};

}  // namespace etrace::collector
