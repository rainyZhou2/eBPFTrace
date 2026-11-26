#include "query/auth.h"

#include <utility>

#include "platform/error.h"

namespace etrace::query {

void decode_fields(config::Fields& f, AuthConfig& c) {
	f("realm", c.realm)("tokens", c.tokens)("basic_user", c.basic_user)("basic_password", c.basic_password)(
	        "allow_unauthenticated", c.allow_unauthenticated);
}

void AuthConfig::check() const {
	for (const auto& token : tokens) {
		if (token.empty()) {
			throw Error("query auth: tokens must not contain an empty value");
		}
	}
	if (basic_user.empty() != basic_password.empty()) {
		throw Error("query auth: basic_user and basic_password must be set together");
	}
	if (realm.empty()) {
		throw Error("query auth: realm must not be empty");
	}
	if (allow_mtls || allow_unauthenticated) {
		return;
	}
	if (tokens.empty() && basic_user.empty()) {
		throw Error("query auth: configure tokens, basic credentials, mTLS, or allow_unauthenticated");
	}
}

Auth::Auth(AuthConfig config) : cfg_(std::move(config)), tokens_(cfg_.tokens) {
	cfg_.check();
	basic_enabled_ = !cfg_.basic_user.empty();
}

bool Auth::authorize(std::string_view authorization) const {
	if (anonymous()) {
		return true;
	}
	if (basic_enabled_) {
		if (auto credentials = auth::basic_credentials(authorization)) {
			if (auth::constant_time_equal(credentials->user, cfg_.basic_user) &&
			    auth::constant_time_equal(credentials->password, cfg_.basic_password)) {
				return true;
			}
		}
	}
	if (!tokens_.empty()) {
		if (auto token = auth::bearer_token(authorization)) {
			return tokens_.contains(*token);
		}
	}
	return false;
}

std::string Auth::challenge() const {
	return basic_enabled_ ? auth::basic_challenge(cfg_.realm) : auth::bearer_challenge(cfg_.realm);
}

}  // namespace etrace::query
