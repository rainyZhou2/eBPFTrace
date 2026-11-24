"use strict";
// All data from the API is untrusted (symbol and process names come from
// traced hosts): it is only ever inserted with textContent.
(function () {
  var form = document.getElementById("filters");
  var errBox = document.getElementById("error");
  var tab = "flame";
  var flameRoot = null;

  function el(tag, text, cls) {
    var e = document.createElement(tag);
    if (text !== undefined && text !== null) e.textContent = String(text);
    if (cls) e.className = cls;
    return e;
  }
  function ms(ns) { return (Number(ns) / 1e6).toFixed(1) + " ms"; }
  function showError(msg) { errBox.hidden = !msg; errBox.textContent = msg || ""; }

  function params(extra) {
    var fd = new FormData(form), q = new URLSearchParams();
    var range = fd.get("range");
    if (range === "custom") {
      if (fd.get("from")) q.set("from", new Date(fd.get("from")).toISOString());
      if (fd.get("to")) q.set("to", new Date(fd.get("to")).toISOString());
    } else {
      var to = new Date(), span = { "15m": 9e5, "1h": 36e5, "6h": 216e5, "24h": 864e5 }[range] || 9e5;
      q.set("from", new Date(to - span).toISOString());
      q.set("to", to.toISOString());
    }
    ["node", "namespace", "pod", "comm", "pid", "depth"].forEach(function (k) {
      var v = (fd.get(k) || "").trim();
      if (v !== "") q.set(k, v);
    });
    Object.keys(extra || {}).forEach(function (k) { if (extra[k] !== "") q.set(k, extra[k]); });
    return q;
  }

  function getJSON(path, q) {
    return fetch(path + "?" + q.toString()).then(function (r) {
      return r.json().catch(function () { return {}; }).then(function (body) {
        if (!r.ok) throw new Error(body.error || r.statusText);
        return body;
      });
    });
  }

  function saveState() {
    var q = new URLSearchParams(new FormData(form));
    q.set("tab", tab);
    var tid = document.getElementById("trace-id").value.trim();
    if (tid) q.set("trace_id", tid);
    history.replaceState(null, "", "#" + q.toString());
  }

  function loadState() {
    var q = new URLSearchParams(location.hash.slice(1));
    q.forEach(function (v, k) {
      if (k === "tab") tab = v;
      else if (k === "trace_id") document.getElementById("trace-id").value = v;
      else if (form.elements[k]) form.elements[k].value = v;
    });
  }

  // ---- flame graph ----
  function color(name) {
    if (name === "--" || name.indexOf("<- ") === 0) return "#d0d7de";
    if (name.indexOf("waiting: ") === 0) return "#c8b6ff";
    var h = 0;
    for (var i = 0; i < name.length; i++) h = (h * 31 + name.charCodeAt(i)) >>> 0;
    return name.indexOf("[kernel]") === 0 ? "hsl(" + (h % 40 + 180) + ",55%,72%)" : "hsl(" + (h % 50 + 10) + ",75%,68%)";
  }

  function drawFlame(focus) {
    var box = document.getElementById("flame-graph"), info = document.getElementById("flame-info");
    var term = document.getElementById("flame-search").value.trim().toLowerCase();
    box.textContent = "";
    if (!flameRoot || !flameRoot.value) { box.appendChild(el("div", "No off-CPU data for these filters.", "empty")); box.style.height = ""; return; }
    var width = box.clientWidth, rowH = 17, maxDepth = 0;
    function place(n, x, w, depth) {
      if (w < 2) return;
      maxDepth = Math.max(maxDepth, depth);
      var d = el("div", n.name);
      d.style.left = x + "px"; d.style.width = w + "px"; d.style.top = depth * rowH + "px";
      d.style.background = color(n.name);
      d.title = n.name + " — " + ms(n.value) + " (" + (100 * n.value / flameRoot.value).toFixed(1) + "%)";
      if (term) d.className = n.name.toLowerCase().indexOf(term) >= 0 ? "match" : "dim";
      d.onclick = function () { drawFlame(depth === 0 ? flameRoot : n); };
      d.onmouseenter = function () { info.textContent = d.title; };
      box.appendChild(d);
      var cx = x;
      (n.children || []).forEach(function (c) { var cw = w * c.value / n.value; place(c, cx, cw, depth + 1); cx += cw; });
    }
    place(focus, 0, width, 0);
    box.style.height = (maxDepth + 1) * rowH + "px";
  }

  function loadFlame() {
    var q = params();
    document.getElementById("dl-pprof").href = "/api/v1/offcpu/flamegraph?" + q + "&format=pprof";
    document.getElementById("dl-collapsed").href = "/api/v1/offcpu/flamegraph?" + q + "&format=collapsed";
    return getJSON("/api/v1/offcpu/flamegraph", q).then(function (tree) { flameRoot = tree; drawFlame(tree); });
  }

  // ---- tables ----
  function fill(section, rows, render, onClick) {
    var tbody = document.querySelector("#" + section + " tbody");
    tbody.textContent = "";
    if (!rows || !rows.length) {
      var tr = el("tr"), td = el("td", "Nothing found for these filters.", "empty");
      td.colSpan = 8; tr.appendChild(td); tbody.appendChild(tr);
      return;
    }
    rows.forEach(function (r) {
      var tr = el("tr");
      render(r).forEach(function (c) {
        var td = el("td", null, c.num ? "num" : "");
        td.appendChild(c.code ? el("code", c.text) : document.createTextNode(c.text == null ? "" : String(c.text)));
        tr.appendChild(td);
      });
      if (onClick) { tr.className = "clickable"; tr.onclick = function () { onClick(r); }; }
      tbody.appendChild(tr);
    });
  }

  function loadCauses() {
    return getJSON("/api/v1/offcpu/causes", params({ limit: "100" })).then(function (body) {
      var labels = { blocked_site: "blocked site", observed_wakeup: "observed wakeup", inferred_upstream: "inferred upstream" };
      fill("causes", body.causes, function (c) {
        var path = c.path.map(function (task, i) { return task + (i === 0 ? "" : i === 1 ? " (observed)" : " (inferred)"); });
        var evidence = labels[c.cause_evidence];
        if (c.cause_evidence === "observed_wakeup" && c.hop_wait_ns[0] > 0) evidence += " (prior wait inferred)";
        return [{ text: path.join(" ← "), code: true }, { text: c.blocked_in, code: true },
          { text: evidence + ": " + c.root_cause, code: true },
          { text: ms(c.total_ns), num: true }, { text: c.count, num: true }];
      });
    });
  }

  function showDetail(text) {
    var d = document.getElementById("detail");
    d.querySelector("pre").textContent = text;
    d.showModal();
  }

  function eventDetail(e) {
    var lines = [e.comm + "[" + e.tid + "] " + e.reason + " " + ms(Date.parse(e.end) * 1e6 - Date.parse(e.start) * 1e6)];
    if (e.reason === "gpu") {
      lines.push("stream " + e.gpu_stream + ", " + e.gpu_launches + " kernels queued since its previous wait");
    }
    lines = lines.concat(["", "blocked in:"], e.stack.map(function (f) { return "  " + f; }));
    (e.chain || []).forEach(function (h) {
      var observed = h.wake_evidence === "observed";
      lines.push("", (observed ? "Observed wakeup by " : "Inferred upstream hop ") + h.comm);
      (h.wake_stack || []).forEach(function (f) { lines.push("  " + f); });
      if (h.wait_evidence === "inferred") {
        lines.push("  Overlapping prior wait (inferred link)" + (h.wait_ns ? ": " + ms(h.wait_ns) : ""));
        (h.wait_stack || []).forEach(function (f) { lines.push("    " + f); });
      }
    });
    if (e.trace_id) lines.push("", "trace " + e.trace_id + " span " + e.span_id);
    showDetail(lines.join("\n"));
  }

  function loadEvents() {
    var md = (form.elements.min_duration.value || "").trim();
    return getJSON("/api/v1/offcpu/events", params({ min_duration: md, limit: "500" })).then(function (body) {
      var evs = (body.events || []).slice().sort(function (a, b) { return Date.parse(b.start) - Date.parse(a.start); });
      fill("events", evs, function (e) {
        return [{ text: new Date(e.start).toLocaleTimeString() }, { text: ms((Date.parse(e.end) - Date.parse(e.start)) * 1e6), num: true },
          { text: e.comm + "[" + e.tid + "]", code: true }, { text: e.namespace ? e.namespace + "/" + e.pod : "" },
          { text: (e.chain || []).map(function (h) { return h.comm + " (" + h.wake_evidence + ")"; }).join(" ← "), code: true },
          { text: e.trace_id || "", code: true }];
      }, eventDetail);
    });
  }

  function loadTrace() {
    var id = document.getElementById("trace-id").value.trim().toLowerCase(), note = document.getElementById("trace-note");
    note.textContent = "";
    if (!id) { fill("trace", [], function () { return []; }); return Promise.resolve(); }
    var q = params();
    q.delete("from"); q.delete("to");
    return getJSON("/api/v1/traces/" + encodeURIComponent(id) + "/offcpu", q).then(function (rep) {
      note.textContent = "off-CPU total " + ms(rep.offcpu_total_ns) + (rep.partial ? " · " + rep.partial : "");
      var spans = rep.spans || [];
      if (!spans.length && rep.unattributed_events) {
        spans = [{ operation: "(unattributed)", service: "", duration_ns: 0, offcpu_ns: rep.offcpu_total_ns, self_offcpu_ns: rep.offcpu_total_ns, events: rep.unattributed_events }];
      }
      fill("trace", spans, function (s) {
        return [{ text: s.operation || s.span_id, code: true }, { text: s.service || "" }, { text: s.duration_ns ? ms(s.duration_ns) : "", num: true },
          { text: ms(s.offcpu_ns), num: true }, { text: ms(s.self_offcpu_ns), num: true }];
      }, function (s) { if (s.events && s.events.length) eventDetail(s.events[0]); });
    });
  }

  var loaders = { flame: loadFlame, causes: loadCauses, events: loadEvents, trace: loadTrace };

  function refresh() {
    showError("");
    saveState();
    loaders[tab]().catch(function (e) { showError(e.message); });
  }

  function selectTab(name) {
    tab = loaders[name] ? name : "flame";
    document.querySelectorAll("#tabs button").forEach(function (b) { b.classList.toggle("active", b.dataset.tab === tab); });
    document.querySelectorAll(".tab").forEach(function (s) { s.classList.toggle("active", s.id === tab); });
    document.body.className = (form.elements.range.value === "custom" ? "custom-range " : "") + "tab-" + tab;
  }

  document.getElementById("tabs").addEventListener("click", function (e) {
    if (e.target.dataset.tab) { selectTab(e.target.dataset.tab); refresh(); }
  });
  form.addEventListener("submit", function (e) { e.preventDefault(); refresh(); });
  form.elements.range.addEventListener("change", function () { selectTab(tab); });
  document.getElementById("flame-search").addEventListener("input", function () { if (flameRoot) drawFlame(flameRoot); });
  document.getElementById("trace-go").addEventListener("click", refresh);
  document.querySelector("#detail .close").addEventListener("click", function () { document.getElementById("detail").close(); });
  window.addEventListener("resize", function () { if (tab === "flame" && flameRoot) drawFlame(flameRoot); });

  loadState();
  selectTab(tab);
  refresh();
})();
