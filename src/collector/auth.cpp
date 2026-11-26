#include "collector/auth.h"

#include <utility>

#include "platform/error.h"

namespace etrace::collector {

void AuthConfig::validate() const {
	for (const auto& token : tokens) {
		if (token.empty()) {
			throw Error("collector auth: tokens must not contain an empty value");
		}
	}
	if (allow_mtls || allow_unauthenticated) {
		return;
	}
	if (tokens.empty()) {
		throw Error("collector auth: tokens are required (set allow_unauthenticated for local testing)");
	}
}

Auth::Auth(AuthConfig config) : cfg_(std::move(config)), tokens_(cfg_.tokens) {
	validate();
}

Auth::Auth(std::vector<std::string> tokens, bool allow_unauthenticated, bool allow_mtls)
    : Auth(AuthConfig{std::move(tokens), allow_unauthenticated, allow_mtls}) {}

void Auth::validate() const {
	cfg_.validate();
}

bool Auth::authorize(std::string_view authorization) const {
	if (anonymous()) {
		return true;
	}
	auto token = auth::bearer_token(authorization);
	return token && tokens_.contains(*token);
}

}  // namespace etrace::collector
