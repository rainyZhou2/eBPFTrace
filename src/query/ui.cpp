#include "query/ui.h"

#include <string_view>

namespace etrace::query::ui::assets {
extern const std::string_view index_html;
extern const std::string_view app_js;
extern const std::string_view app_css;
}  // namespace etrace::query::ui::assets

namespace etrace::query::ui {

namespace {

constexpr const char* kCSP =
        "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self'; base-uri 'none'; "
        "form-action 'none'; frame-ancestors 'none'";

void serve(httplib::Response& res, std::string_view body, const char* type) {
	res.set_header("Content-Security-Policy", kCSP);
	res.set_header("X-Content-Type-Options", "nosniff");
	res.set_header("Referrer-Policy", "no-referrer");
	res.set_content(body.data(), body.size(), type);
}

}  // namespace

void routes(httplib::Server& srv) {
	srv.Get("/", [](const httplib::Request&, httplib::Response& res) {
		serve(res, assets::index_html, "text/html; charset=utf-8");
	});
	srv.Get("/ui/index.html", [](const httplib::Request&, httplib::Response& res) {
		serve(res, assets::index_html, "text/html; charset=utf-8");
	});
	srv.Get("/ui/app.js", [](const httplib::Request&, httplib::Response& res) {
		serve(res, assets::app_js, "text/javascript; charset=utf-8");
	});
	srv.Get("/ui/app.css", [](const httplib::Request&, httplib::Response& res) {
		serve(res, assets::app_css, "text/css; charset=utf-8");
	});
}

}  // namespace etrace::query::ui
