#pragma once

#include <httplib.h>

namespace etrace::query::ui {

// routes serves the browser front end: a static page that calls the JSON
// API, embedded in the binary. The page loads only its own script and
// style and talks only to the same origin, so a strict content security
// policy costs nothing and contains injected markup.
void routes(httplib::Server& srv);

}  // namespace etrace::query::ui
