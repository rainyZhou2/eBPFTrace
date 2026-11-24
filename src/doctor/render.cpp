#include <format>
#include <string>

#include "doctor/report.h"
#include "query/json.h"

namespace etrace::doctor {

namespace {

std::string ms(uint64_t ns) {
	return std::format("{:.1f}ms", static_cast<double>(ns) / 1e6);
}

std::string join(const std::vector<std::string>& v, std::string_view sep) {
	std::string out;
	for (size_t i = 0; i < v.size(); i++) {
		if (i > 0) {
			out += sep;
		}
		out += v[i];
	}
	return out;
}

std::string wake_path(std::vector<std::string> path, size_t first_hop, std::string_view sep) {
	for (size_t i = first_hop; i < path.size(); i++) {
		path[i] += i == first_hop ? " (observed)" : " (inferred)";
	}
	return join(path, sep);
}

std::string chain_summary(const storage::Event& e, std::string_view sep) {
	std::vector<std::string> path;
	for (const auto& h : e.chain) {
		path.push_back(h.comm);
	}
	return wake_path(std::move(path), 0, sep);
}

std::string_view cause_label(const query::CausePath& cause) {
	switch (cause.evidence) {
		case query::CauseEvidence::blocked_site:
			return "blocked site";
		case query::CauseEvidence::observed_wakeup:
			return !cause.hops_wait.empty() && cause.hops_wait.front() != 0
			               ? "observed wakeup (prior wait inferred)"
			               : "observed wakeup";
		case query::CauseEvidence::inferred_upstream:
			return "inferred upstream";
	}
	return {};
}

// esc escapes text for HTML element content and quoted attribute values.
std::string esc(std::string_view s) {
	std::string out;
	out.reserve(s.size());
	for (char c : s) {
		switch (c) {
			case '&':
				out += "&amp;";
				break;
			case '<':
				out += "&lt;";
				break;
			case '>':
				out += "&gt;";
				break;
			case '"':
				out += "&#34;";
				break;
			case '\'':
				out += "&#39;";
				break;
			case '\n':
				out += "&#10;";
				break;
			case '\0':
				out += "\xef\xbf\xbd";
				break;
			default:
				out += c;
		}
	}
	return out;
}

std::string ts(Time t) {
	return format_local(t, "%Y-%m-%d %H:%M:%S");
}

constexpr const char* kHead = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>etrace doctor · )HTML";

constexpr const char* kStyle = R"HTML(</title>
<style>
body { font: 14px/1.45 -apple-system, "Segoe UI", Roboto, sans-serif; margin: 24px; color: #1f2328; }
h1 { font-size: 20px; margin: 0 0 4px; }
h2 { font-size: 16px; margin: 28px 0 8px; border-bottom: 1px solid #d0d7de; padding-bottom: 4px; }
.meta { color: #59636e; }
table { border-collapse: collapse; width: 100%; }
th, td { text-align: left; padding: 4px 8px; border-bottom: 1px solid #eaeef2; vertical-align: top; }
th { background: #f6f8fa; font-weight: 600; }
td.num { text-align: right; white-space: nowrap; font-variant-numeric: tabular-nums; }
code { font: 12px ui-monospace, SFMono-Regular, Menlo, monospace; word-break: break-all; }
.ok { color: #1a7f37; } .warn { color: #9a6700; } .fail { color: #cf222e; }
.status { font-weight: 600; text-transform: uppercase; white-space: nowrap; }
#flame { position: relative; overflow: hidden; border: 1px solid #d0d7de; }
#flame div { position: absolute; height: 17px; box-sizing: border-box; border: 1px solid #fff;
  font: 11px/15px ui-monospace, Menlo, monospace; padding: 0 3px; overflow: hidden; white-space: nowrap; cursor: pointer; }
#flame-info { min-height: 20px; margin: 6px 0; }
</style>
</head>
<body>
<h1>etrace doctor</h1>
)HTML";

constexpr const char* kFlameIntro = R"HTML(<h2>Flame graph</h2>
<p class="meta">Root at the top. The first <code>&lt;- task</code> is the observed wakeup. Subsequent hops and <code>waiting: task</code> frames are linked by overlap. Click a frame to zoom, click the top row to reset.</p>
<div id="flame-info"></div>
<div id="flame"></div>
<script id="flame-data" type="application/json">)HTML";

constexpr const char* kFlameScript = R"HTML(</script>
<script>
(function () {
  var root = JSON.parse(document.getElementById("flame-data").textContent);
  var box = document.getElementById("flame"), info = document.getElementById("flame-info");
  var rowH = 17, minPx = 2;
  function color(name) {
    if (name === "--" || name.indexOf("<- ") === 0) return "#d0d7de";
    if (name.indexOf("waiting: ") === 0) return "#c8b6ff";
    var h = 0;
    for (var i = 0; i < name.length; i++) h = (h * 31 + name.charCodeAt(i)) >>> 0;
    return name.indexOf("[kernel]") === 0 ? "hsl(" + (h % 40 + 180) + ",55%,72%)" : "hsl(" + (h % 50 + 10) + ",75%,68%)";
  }
  function fmt(ns) { return (ns / 1e6).toFixed(1) + "ms"; }
  function draw(focus) {
    box.textContent = "";
    var width = box.clientWidth, maxDepth = 0;
    function place(n, x, w, depth) {
      if (w < minPx) return;
      maxDepth = Math.max(maxDepth, depth);
      var d = document.createElement("div");
      d.style.left = x + "px"; d.style.width = w + "px"; d.style.top = depth * rowH + "px";
      d.style.background = color(n.name);
      d.textContent = n.name;
      d.title = n.name + " — " + fmt(n.value) + " (" + (100 * n.value / root.value).toFixed(1) + "%)";
      d.onclick = function () { draw(depth === 0 ? root : n); };
      d.onmouseenter = function () { info.textContent = d.title; };
      box.appendChild(d);
      var cx = x;
      (n.children || []).forEach(function (c) {
        var cw = w * c.value / n.value;
        place(c, cx, cw, depth + 1);
        cx += cw;
      });
    }
    place(focus, 0, width, 0);
    box.style.height = (maxDepth + 1) * rowH + "px";
  }
  if (root.value > 0) { draw(root); window.addEventListener("resize", function () { draw(root); }); }
})();
</script>
)HTML";

}  // namespace

void write_html(std::ostream& out, const Report& r) {
	out << kHead << esc(r.host) << kStyle;
	out << "<div class=\"meta\">host <b>" << esc(r.host) << "</b> · kernel " << esc(r.kernel) << " · etrace "
	    << esc(r.version) << " · " << esc(ts(r.start)) << " – " << esc(ts(r.end));
	if (r.interrupted) {
		out << " · stopped early";
	}
	out << "<br>target: " << esc(r.target) << "</div>\n\n<h2>Checks</h2>\n<table>\n";
	for (const auto& c : r.checks) {
		auto st = to_string(c.status);
		out << "<tr><td class=\"status " << st << "\">" << st << "</td><td>" << esc(c.name) << "</td><td>"
		    << esc(c.detail) << "</td></tr>\n";
	}
	out << "</table>\n\n";
	if (!r.blocked.empty()) {
		out << "<h2>Still blocked when recording stopped</h2>\n<p class=\"meta\">These waits had not ended, so "
		       "they "
		       "are not in the totals below. A thread blocked for the whole run is a hang "
		       "candidate.</p>\n<table>\n"
		       "<tr><th class=\"num\">blocked for</th><th>thread</th><th>blocked in</th><th>trace</th></tr>\n";
		for (const auto& t : r.blocked) {
			out << "<tr><td class=\"num\">" << esc(ms(static_cast<uint64_t>(t.blocked_for.count())))
			    << "+</td><td><code>" << esc(t.comm) << "[" << t.tid << "]</code></td><td><code title=\""
			    << esc(join(t.stack, "\n")) << "\">" << esc(t.at) << "</code></td><td><code>"
			    << esc(t.trace_id) << "</code></td></tr>\n";
		}
		out << "</table>";
	}
	out << "\n\n<h2>Wait time of finished waits (off-CPU and GPU): " << esc(ms(r.total_ns)) << "</h2>\n";
	if (r.total_ns == 0) {
		out << "<p>Nothing blocked longer than the threshold during the run.</p>\n</body>\n</html>\n";
		return;
	}
	out << "<table>\n<tr><th>process</th><th class=\"num\">waiting</th><th class=\"num\">share</th></tr>\n";
	for (const auto& p : r.processes) {
		out << "<tr><td><code>" << esc(p.comm) << "</code></td><td class=\"num\">" << esc(ms(p.total_ns))
		    << "</td><td class=\"num\">" << std::format("{:.1f}%", p.share * 100) << "</td></tr>\n";
	}
	out << "</table>\n\n<h2>Top wait paths</h2>\n<p class=\"meta\">The first wakeup was observed. Upstream links "
	       "are inferred from overlapping waits.</p>\n<table>\n<tr><th>wait "
	       "path</th><th>blocked in</th><th>site and evidence</th><th class=\"num\">total</th><th "
	       "class=\"num\">count</th></tr>\n";
	for (const auto& c : r.causes) {
		out << "<tr><td><code>" << esc(wake_path(c.path, 1, " ← ")) << "</code></td><td><code>"
		    << esc(c.blocked) << "</code></td><td><code>" << cause_label(c) << ": " << esc(c.root)
		    << "</code></td><td class=\"num\">" << esc(ms(c.total_ns)) << "</td><td class=\"num\">" << c.count
		    << "</td></tr>\n";
	}
	out << "</table>\n\n";
	if (!r.slow.empty()) {
		out << "<h2>Slowest waits</h2>\n<table>\n<tr><th class=\"num\">duration</th><th>thread</th><th>blocked "
		       "in</th><th>wake path</th><th>trace</th></tr>\n";
		for (const auto& e : r.slow) {
			out << "<tr><td class=\"num\">" << esc(ms(static_cast<uint64_t>((e.end - e.start).count())))
			    << "</td><td><code>" << esc(e.comm) << "[" << e.tid << "]</code></td><td><code>"
			    << esc(query::blocked_frame(e.stack)) << "</code></td><td><code>"
			    << esc(chain_summary(e, " ← ")) << "</code></td><td><code>" << esc(e.trace_id)
			    << "</code></td></tr>\n";
		}
		out << "</table>";
	}
	out << "\n\n" << kFlameIntro;
	out << (r.flame ? query::html_safe_dump(query::flame::to_json(*r.flame)) : "null");
	out << kFlameScript << "\n</body>\n</html>\n";
}

void write_text(std::ostream& out, const Report& r) {
	std::string b = std::format("etrace doctor {}  host={} kernel={}  {} .. {}", r.version, r.host, r.kernel,
	                            format_local(r.start, "%H:%M:%S"), format_local(r.end, "%H:%M:%S"));
	if (r.interrupted) {
		b += "  (stopped early)";
	}
	b += std::format("\ntarget: {}\n\nchecks:\n", r.target);
	for (const auto& c : r.checks) {
		b += std::format("  [{:<4}] {:<16} {}\n", to_string(c.status), c.name, c.detail);
	}
	if (!r.blocked.empty()) {
		b += "\nstill blocked when recording stopped:\n";
		for (const auto& t : r.blocked) {
			b += std::format("  {:>10}+ {}[{}]  {}", ms(static_cast<uint64_t>(t.blocked_for.count())),
			                 t.comm, t.tid, t.at);
			if (!t.trace_id.empty()) {
				b += std::format("  trace={}", t.trace_id);
			}
			b += '\n';
		}
	}
	b += std::format("\nwait time of finished waits (off-CPU and GPU): {}\n", ms(r.total_ns));
	if (r.total_ns == 0) {
		b += "  nothing blocked longer than the threshold during the run\n";
		out << b;
		return;
	}
	b += "\nprocesses:\n";
	for (const auto& p : r.processes) {
		b += std::format("  {:>10} {:6.1f}%  {}\n", ms(p.total_ns), p.share * 100, p.comm);
	}
	b += "\ntop wait paths (upstream links inferred from overlapping waits):\n";
	for (const auto& c : r.causes) {
		b += std::format("  {:>10} {:6}x  {}\n      blocked in {}\n      {}: {}\n", ms(c.total_ns), c.count,
		                 wake_path(c.path, 1, " <- "), c.blocked, cause_label(c), c.root);
	}
	if (!r.slow.empty()) {
		b += "\nslowest waits:\n";
		for (const auto& e : r.slow) {
			b += std::format("  {:>10}  {}[{}]  {}", ms(static_cast<uint64_t>((e.end - e.start).count())),
			                 e.comm, e.tid, query::blocked_frame(e.stack));
			if (auto c = chain_summary(e, " <- "); !c.empty()) {
				b += std::format("  <- {}", c);
			}
			if (!e.trace_id.empty()) {
				b += std::format("  trace={} span={}", e.trace_id, e.span_id);
			}
			b += '\n';
		}
	}
	out << b;
}

}  // namespace etrace::doctor
