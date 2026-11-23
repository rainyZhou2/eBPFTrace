#pragma once

#include <nlohmann/json.hpp>

#include "storage/storage.h"

namespace etrace::query {

using Json = nlohmann::ordered_json;

Json to_json(const storage::Hop& h);
Json to_json(const storage::Event& e);
Json to_json(const std::vector<storage::Event>& events);

// html_safe_dump escapes <, > and & so JSON can be embedded in HTML
// script elements, as encoding/json does for Go.
std::string html_safe_dump(const Json& j);

}  // namespace etrace::query
