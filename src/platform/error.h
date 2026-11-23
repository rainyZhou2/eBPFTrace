#pragma once

#include <stdexcept>
#include <string>

namespace etrace {

// Error is the base of every failure raised by eTrace code. Modules derive
// their own types when callers need to tell failures apart.
class Error : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

}  // namespace etrace
