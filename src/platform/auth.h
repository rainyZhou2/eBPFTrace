#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace etrace::auth {

bool constant_time_equal(std::string_view a, std::string_view b);

class TokenSet {
public:
	explicit TokenSet(std::vector<std::string> tokens);

	bool empty() const { return tokens_.empty(); }
	bool contains(std::string_view token) const;

private:
	std::vector<std::string> tokens_;
};

// bearer_token returns the credential of an "authorization: Bearer ..."
// header, or nothing for another scheme.
std::optional<std::string> bearer_token(std::string_view header);

struct BasicCredentials {
	std::string user;
	std::string password;
};

std::optional<BasicCredentials> basic_credentials(std::string_view header);

std::string basic_challenge(std::string_view realm);
std::string bearer_challenge(std::string_view realm);

}  // namespace etrace::auth
