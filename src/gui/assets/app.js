/* HCS Studio (HCSmedia) local UI. Vanilla JS, no dependencies, no inline handlers, no network access except same-origin API calls.
   Everything shown comes from the server; unsupported features display the server's reason. */
"use strict";
(function () {
  var $ = function (id) { return document.getElementById(id); };
  var DEFAULTS = { interval: 3 };
  var TITLES = { chat: "Chat", image: "Image Studio", vision: "Vision", models: "Models", perf: "Performance", settings: "Settings", agent: "Agent", train: "Train" };
  var state = { token: "", mode: "simple", models: [], runtime: null, jobs: {}, perfTimer: 0, busy: {} };

  /* ---------- small helpers ---------- */
  function store(k, v) { try { if (v === undefined) return sessionStorage.getItem(k); if (v === null) sessionStorage.removeItem(k); else sessionStorage.setItem(k, v); } catch (e) { /* storage may be blocked */ } return null; }
  function lstore(k, v) { try { if (v === undefined) return localStorage.getItem(k); localStorage.setItem(k, v); } catch (e) { /* ignore */ } return null; }
  function text(node, s) { node.textContent = s == null ? "" : String(s); return node; }
  function el(tag, cls, s) { var n = document.createElement(tag); if (cls) n.className = cls; if (s != null) n.textContent = String(s); return n; }
  function clear(n) { while (n.firstChild) n.removeChild(n.firstChild); }
  function fmtBytes(n) {
    if (typeof n !== "number") return String(n);
    var u = ["B", "KiB", "MiB", "GiB", "TiB"], i = 0;
    while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
    return (i ? n.toFixed(1) : String(n)) + " " + u[i];
  }
  function fmtVal(k, v) {
    if (v === null || v === undefined) return "unknown";
    if (typeof v === "number") return /bytes$/.test(k) ? fmtBytes(v) + " (" + v + ")" : (Math.round(v * 100) / 100).toString();
    if (typeof v === "object") return JSON.stringify(v);
    return String(v);
  }
  function fillDl(dl, obj, prefix) {
    Object.keys(obj).forEach(function (k) {
      var v = obj[k];
      if (v && typeof v === "object" && !Array.isArray(v)) { fillDl(dl, v, (prefix ? prefix + "." : "") + k); return; }
      dl.appendChild(el("dt", "", (prefix ? prefix + "." : "") + k));
      dl.appendChild(el("dd", "", fmtVal(k, v)));
    });
  }
  function randomSeed() {
    try { var a = new Uint32Array(1); crypto.getRandomValues(a); return a[0] % 2147483647; } catch (e) { return Math.floor(Math.random() * 2147483647); }
  }
  function dataUriToBlob(uri) {
    var m = /^data:([^;,]+)?(;base64)?,(.*)$/.exec(uri);
    if (!m) return null;
    var bin = m[2] ? atob(m[3]) : decodeURIComponent(m[3]), arr = new Uint8Array(bin.length);
    for (var i = 0; i < bin.length; i++) arr[i] = bin.charCodeAt(i);
    return new Blob([arr], { type: m[1] || "application/octet-stream" });
  }
  function readFileAsDataUri(f, ok, fail) {
    var fr = new FileReader();
    fr.onload = function () { ok(String(fr.result)); };
    fr.onerror = function () { fail(); };
    fr.readAsDataURL(f);
  }
  var IMG_OK = /^image\/(png|jpeg|gif|bmp)$/;

  /* ---------- toast + clipboard ---------- */
  var toastTimer = 0;
  function toast(msg) {
    var t = $("toast");
    if (!t) return;
    text(t, msg); t.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { t.hidden = true; }, 2400);
  }
  async function copyText(s, what) {
    var ok = false;
    try { await navigator.clipboard.writeText(s); ok = true; } catch (e) { ok = false; }
    if (!ok) {
      /* Fallback for contexts where the async clipboard is refused: a transient selection. */
      try {
        var ta = el("textarea"); ta.value = s; ta.setAttribute("readonly", ""); ta.className = "vh";
        document.body.appendChild(ta); ta.select(); ok = document.execCommand("copy"); document.body.removeChild(ta);
      } catch (e2) { ok = false; }
    }
    toast(ok ? (what || "Copied") + " copied to the clipboard." : "Copy was refused by the browser. Select the text and press Ctrl+C.");
  }
  function copyButton(getText, what, label) {
    var b = el("button", "ghost copy", "Copy");
    b.type = "button";
    b.setAttribute("aria-label", label || ("Copy " + (what || "text")));
    b.addEventListener("click", function () { copyText(getText(), what); });
    return b;
  }

  /* ---------- safe Markdown (DOM nodes only; model output is never parsed as HTML) ---------- */
  var INLINE = /(`[^`\n]+`)|(\*\*[^*\n]+\*\*)|(__[^_\n]+__)|(\*[^*\s][^*\n]*\*)|(\[[^\]\n]+\]\(https?:\/\/[^\s)]+\))/g;
  function inlineMd(parent, s) {
    var last = 0, m;
    INLINE.lastIndex = 0;
    while ((m = INLINE.exec(s)) !== null) {
      if (m.index > last) parent.appendChild(document.createTextNode(s.slice(last, m.index)));
      var tok = m[0];
      if (m[1]) parent.appendChild(el("code", "", tok.slice(1, -1)));
      else if (m[2] || m[3]) parent.appendChild(el("strong", "", tok.slice(2, -2)));
      else if (m[4]) parent.appendChild(el("em", "", tok.slice(1, -1)));
      else if (m[5]) {
        var lm = /^\[([^\]]+)\]\((https?:\/\/[^\s)]+)\)$/.exec(tok);
        var a = el("a", "", lm[1]);
        a.href = lm[2]; a.target = "_blank"; a.rel = "noopener noreferrer nofollow"; a.title = lm[2];
        parent.appendChild(a);
      }
      last = m.index + tok.length;
    }
    if (last < s.length) parent.appendChild(document.createTextNode(s.slice(last)));
  }
  function codeBlock(parent, lang, code) {
    var wrap = el("div", "codeblock"), head = el("div", "codehead"), pre = el("pre"), c = el("code", "", code);
    head.appendChild(el("span", "lang", lang || "text"));
    head.appendChild(copyButton(function () { return code; }, "code", "Copy code block"));
    pre.tabIndex = 0; pre.setAttribute("aria-label", "Code block" + (lang ? " (" + lang + ")" : ""));
    pre.appendChild(c); wrap.appendChild(head); wrap.appendChild(pre); parent.appendChild(wrap);
  }
  function renderMd(root, src) {
    clear(root);
    var lines = String(src).replace(/\r\n?/g, "\n").split("\n"), i = 0, para = [];
    function flush() { if (para.length) { var p = el("p"); inlineMd(p, para.join("\n")); root.appendChild(p); para = []; } }
    while (i < lines.length) {
      var ln = lines[i], m;
      if ((m = /^ {0,3}```\s*([A-Za-z0-9_+#.-]*)\s*$/.exec(ln))) {          /* fenced block; an unclosed fence (still streaming) runs to the end */
        flush();
        var buf = []; i++;
        while (i < lines.length && !/^ {0,3}```\s*$/.test(lines[i])) { buf.push(lines[i]); i++; }
        i++;
        codeBlock(root, m[1], buf.join("\n"));
        continue;
      }
      if ((m = /^(#{1,4})\s+(.*)$/.exec(ln))) { flush(); var h = el("h" + (m[1].length + 2)); h.className = "mdh"; inlineMd(h, m[2]); root.appendChild(h); i++; continue; }
      if (/^ {0,3}([-*_])( *\1){2,} *$/.test(ln)) { flush(); root.appendChild(el("hr")); i++; continue; }
      if (/^ {0,3}>\s?/.test(ln)) {
        flush();
        var q = [];
        while (i < lines.length && /^ {0,3}>\s?/.test(lines[i])) { q.push(lines[i].replace(/^ {0,3}>\s?/, "")); i++; }
        var bq = el("blockquote"); inlineMd(bq, q.join("\n")); root.appendChild(bq);
        continue;
      }
      var lm = /^ {0,3}([-*+]|\d{1,9}[.)])\s+(.*)$/.exec(ln);
      if (lm) {
        flush();
        var ordered = /\d/.test(lm[1]), list = el(ordered ? "ol" : "ul");
        while (i < lines.length && (lm = /^ {0,3}([-*+]|\d{1,9}[.)])\s+(.*)$/.exec(lines[i])) && /\d/.test(lm[1]) === ordered) {
          var li = el("li"); inlineMd(li, lm[2]); list.appendChild(li); i++;
        }
        root.appendChild(list);
        continue;
      }
      if (/^\s*$/.test(ln)) { flush(); i++; continue; }
      para.push(ln); i++;
    }
    flush();
  }

  /* ---------- panel states: idle | busy | loading | error | oom | unsupported | ok ---------- */
  function setBanner(id, kind, msg) {
    var b = $(id);
    if (!b) return;
    if (!kind || kind === "idle") { b.hidden = true; text(b, ""); return; }
    b.hidden = false;
    b.className = "banner banner-" + (kind === "loading" ? "busy" : kind);
    var label = { busy: "Working", loading: "Loading", error: "Error", oom: "Out of memory", unsupported: "Unsupported", ok: "Done" }[kind] || "";
    text(b, (label ? label + ": " : "") + msg);
    if (kind === "oom") b.appendChild(el("div", "note", "Try a smaller quantization, a shorter context, a smaller image, or unload the other model first."));
  }

  /* ---------- API ---------- */
  function ApiError(status, body, fallback) {
    var e = body && body.error ? body.error : {};
    this.status = status;
    this.code = e.code || "";
    this.message = e.message || fallback || ("HTTP " + status);
    this.kind = classify(status, this.code);
  }
  function classify(status, code) {
    if (status === 401) return "auth";
    if (status === 501 || code === "unsupported") return "unsupported";
    if (status === 503 || code === "out_of_memory") return "oom";
    return "error";
  }
  function authHeaders(extra) {
    var h = extra || {};
    h["Authorization"] = "Bearer " + state.token;
    return h;
  }
  async function api(method, path, body, signal) {
    var init = { method: method, headers: authHeaders({}), signal: signal, cache: "no-store", credentials: "omit" };
    if (body !== undefined) { init.headers["Content-Type"] = "application/json"; init.body = JSON.stringify(body); }
    var r;
    try { r = await fetch(path, init); } catch (e) { if (e && e.name === "AbortError") throw e; setConn(false, "server unreachable"); throw new ApiError(0, null, "server unreachable: " + e.message); }
    var j = null, raw = "";
    try { raw = await r.text(); j = raw ? JSON.parse(raw) : null; } catch (e) { j = null; }
    if (r.status === 401) needToken("The server rejected the token.");
    if (!r.ok) throw new ApiError(r.status, j, raw.slice(0, 200));
    return j;
  }
  function describe(e) {
    if (e && e.name === "AbortError") return { kind: "idle", message: "Cancelled." };
    if (e instanceof ApiError) {
      var m = e.message;
      if (e.status === 413) m += " (raise the server's --max-body-bytes or use a smaller input)";
      if (e.status === 429) return { kind: "busy", message: "The server is busy (too many concurrent requests). Try again in a moment." };
      if (e.status === 409 && e.code === "invalid_state") m += " (load a model on the Models tab first)";
      return { kind: e.kind === "auth" ? "error" : e.kind, message: m };
    }
    return { kind: "error", message: (e && e.message) || String(e) };
  }

  /* ---------- connection / token ---------- */
  function setConn(ok, label) {
    var c = $("conn");
    c.className = "pill " + (ok ? "pill-ok" : "pill-err");
    text(c, label);
  }
  function needToken(why) {
    $("authbar").hidden = false;
    setConn(false, "not authenticated");
    if (why) text($("auth-note"), why + " Paste the token printed by HCSInfer serve. It is never put in a URL query string.");
  }
  function takeTokenFromFragment() {
    var h = location.hash || "";
    var m = /(?:^#|&)token=([^&]+)/.exec(h);
    if (m) {
      try { state.token = decodeURIComponent(m[1]); } catch (e) { state.token = m[1]; }
      store("hcs.token", state.token);
      try { history.replaceState(null, "", location.pathname); } catch (e) { location.hash = ""; }
    } else {
      state.token = store("hcs.token") || "";
    }
  }
  async function connect() {
    if (!state.token) { needToken(""); return false; }
    try {
      var r = await fetch("/hcs/v1/health", { cache: "no-store", credentials: "omit" });
      var j = await r.json();
      text($("ver"), "HCSInfer " + (j.version || "") + " - local UI");
    } catch (e) { setConn(false, "server unreachable"); return false; }
    try {
      await refreshRuntime();
      $("authbar").hidden = true;
      setConn(true, "connected");
      await refreshModels();
      return true;
    } catch (e) { return false; }
  }

  /* ---------- theme ---------- */
  var THEMES = ["system", "light", "dark"];
  function setTheme(t) {
    if (THEMES.indexOf(t) < 0) t = "system";
    var root = document.documentElement;
    if (t === "system") root.removeAttribute("data-theme"); else root.setAttribute("data-theme", t);
    lstore("hcs.theme", t);
    var b = $("theme-toggle");
    b.setAttribute("aria-label", "Theme: " + (t === "system" ? "follow system" : t) + ". Activate to change.");
    text($("theme-label"), t);
    Array.prototype.forEach.call(document.querySelectorAll("input[name=theme]"), function (r) { r.checked = r.value === t; });
    state.theme = t;
  }

  /* ---------- tabs / mode / nav ---------- */
  var panels = ["chat", "image", "vision", "models", "perf", "settings", "agent", "train"];
  function closeNav() { $("side").classList.remove("open"); $("nav-toggle").setAttribute("aria-expanded", "false"); }
  function showPanel(name) {
    panels.forEach(function (p) {
      var on = p === name;
      $("panel-" + p).hidden = !on;
      var t = $("tab-" + p);
      t.setAttribute("aria-selected", on ? "true" : "false");
      t.tabIndex = on ? 0 : -1;
    });
    text($("page-title"), TITLES[name] || name);
    document.title = (TITLES[name] || "Studio") + " - HCS Studio";
    closeNav();
    if (name === "perf") startPerf(); else stopPerf();
    if (name === "models") refreshModels().catch(function () {});
    if (name === "image") refreshLoras().catch(function () {});
  }
  function initTabs() {
    var tabs = $("tabs");
    tabs.addEventListener("click", function (e) { var b = e.target.closest("[role=tab]"); if (b) showPanel(b.dataset.panel); });
    tabs.addEventListener("keydown", function (e) {
      var i = panels.indexOf(document.activeElement && document.activeElement.dataset ? document.activeElement.dataset.panel : "");
      if (i < 0) return;
      var n = (e.key === "ArrowRight" || e.key === "ArrowDown") ? i + 1 : (e.key === "ArrowLeft" || e.key === "ArrowUp") ? i - 1 : e.key === "Home" ? 0 : e.key === "End" ? panels.length - 1 : -1;
      if (n < 0) return;
      e.preventDefault();
      n = (n + panels.length) % panels.length;
      showPanel(panels[n]);
      $("tab-" + panels[n]).focus();
    });
    $("nav-toggle").addEventListener("click", function () {
      var open = !$("side").classList.contains("open");
      $("side").classList.toggle("open", open);
      $("nav-toggle").setAttribute("aria-expanded", open ? "true" : "false");
    });
    document.addEventListener("keydown", function (e) { if (e.key === "Escape") closeNav(); });
    $("main").addEventListener("click", closeNav);
  }
  function setMode(m) {
    state.mode = m === "advanced" ? "advanced" : "simple";
    lstore("hcs.mode", state.mode);
    Array.prototype.forEach.call(document.querySelectorAll("[data-advanced]"), function (n) { n.hidden = state.mode !== "advanced"; });
    Array.prototype.forEach.call(document.querySelectorAll("input[name=mode], input[name=ob-mode]"), function (r) { r.checked = r.value === state.mode; });
  }

  /* ---------- models ---------- */
  function kindOf(m) { return m.kind || "unknown"; }
  function fillSelect(sel, kinds) {
    var prev = sel.value;
    clear(sel);
    var list = state.models.filter(function (m) { return kinds.indexOf(kindOf(m)) >= 0 && !/^mmproj/i.test(m.id); });  /* projector files are not models */
    var loaded = state.runtime && state.runtime.loaded_model;
    sel.appendChild(Object.assign(el("option", "", loaded && list.some(function (m) { return m.id === loaded; }) ? "Loaded model (" + loaded + ")" : "Use loaded model"), { value: "" }));
    list.forEach(function (m) { var o = el("option", "", m.id + " [" + m.state + "]"); o.value = m.id; sel.appendChild(o); });
    if (Array.prototype.some.call(sel.options, function (o) { return o.value === prev; })) sel.value = prev;
  }
  function renderModels() {
    fillSelect($("chat-model"), ["text", "vision", "unknown"]);
    fillSelect($("vision-model"), ["vision", "text", "unknown"]);
    fillSelect($("image-model"), ["image"]);
    var tb = $("models-table").tBodies[0];
    clear(tb);
    if (!state.models.length) {
      var tr0 = el("tr"), td0 = el("td", "", "No models registered. Run HCSInfer models scan <folder> on the server.");
      td0.colSpan = 6; tr0.appendChild(td0); tb.appendChild(tr0);
    }
    state.models.forEach(function (m) {
      var tr = el("tr");
      var nm = el("td", "mname");
      if (m.display_name && m.display_name !== m.id) { nm.appendChild(document.createTextNode(m.display_name)); nm.appendChild(el("div", "sub", m.id)); }
      else nm.appendChild(document.createTextNode(m.id));
      tr.appendChild(nm);
      tr.appendChild(el("td", "", /^mmproj/i.test(m.id) ? "projector" : kindOf(m)));
      tr.appendChild(el("td", "", m.quantization || "unknown"));
      tr.appendChild(el("td", "", fmtBytes(m.size_bytes)));
      tr.appendChild(el("td", "", m.state));
      var act = el("td");
      var loading = state.busy["load:" + m.id];
      var isLoaded = m.state === "loaded" || m.state === "running";
      var lb = el("button", "", loading ? "Loading..." : "Load");
      var isProj = /^mmproj/i.test(m.id);  /* a projector is loaded together with its model, never on its own */
      lb.disabled = !!loading || isLoaded || isProj;
      if (isProj) lb.title = "Projector file: it is loaded automatically with its vision model.";
      lb.addEventListener("click", function () { loadModel(m.id); });
      act.appendChild(lb);
      if (loading && loading.jobId) {
        var cb = el("button", "danger", "Cancel");
        cb.addEventListener("click", function () { cancelJob(loading.jobId); });
        act.appendChild(cb);
      }
      var ub = el("button", "ghost", "Unload");
      ub.disabled = !isLoaded || !!loading;
      ub.addEventListener("click", function () { unloadModel(m.id); });
      act.appendChild(ub);
      tr.appendChild(act);
      tb.appendChild(tr);
    });
  }
  /* Memory split: the server's budget entries grouped by category against the budget total. Unknown stays unknown. */
  function renderMemory(mem, pre) {
    pre = pre || "mem";
    var bar = $(pre + "-bar"), lg = $(pre + "-legend"), note = $(pre + "-note");
    clear(bar); clear(lg);
    if (!mem) { text(note, "Memory report unavailable."); return; }
    var total = typeof mem.total_bytes === "number" ? mem.total_bytes : 0;
    var cats = {}, order = [], used = 0;
    (mem.entries || []).forEach(function (e) {
      if (!(e.category in cats)) { cats[e.category] = 0; order.push(e.category); }
      cats[e.category] += e.bytes; used += e.bytes;
    });
    var basis = total || used;
    order.forEach(function (c, i) {
      if (!basis || !cats[c]) return;
      var seg = el("span", "s" + (i % 6));
      seg.style.width = (100 * cats[c] / basis) + "%";
      seg.title = c + ": " + fmtBytes(cats[c]);
      bar.appendChild(seg);
      var li = el("li"); li.appendChild(el("i", "s" + (i % 6))); li.appendChild(document.createTextNode(c + " " + fmtBytes(cats[c])));
      lg.appendChild(li);
    });
    if (total) {
      var li2 = el("li"); li2.appendChild(el("i", "sfree"));
      li2.appendChild(document.createTextNode("free " + fmtBytes(Math.max(0, total - used))));
      lg.appendChild(li2);
    }
    bar.setAttribute("aria-label", "Memory budget: " + (total ? fmtBytes(used) + " reserved of " + fmtBytes(total) : "total unknown, " + fmtBytes(used) + " reserved"));
    var parts = [];
    parts.push(total ? "Budget " + fmtBytes(total) + ", reserved " + fmtBytes(used) + "." : "Budget total unknown on this build; only reservations are shown.");
    if (mem.measured && typeof mem.measured.available_physical_bytes === "number") parts.push("Available physical memory now: " + fmtBytes(mem.measured.available_physical_bytes) + ".");
    if (!order.length) parts.push("Nothing is reserved: no model is loaded.");
    text(note, parts.join(" "));
  }
  async function refreshModels() {
    var j = await api("GET", "/hcs/v1/models");
    state.models = (j && j.models) || [];
    await refreshRuntime().catch(function () {});
    renderModels();
    renderOnboard();
    api("GET", "/hcs/v1/memory").then(renderMemory).catch(function () { renderMemory(null); });
  }
  async function refreshRuntime() {
    var j = await api("GET", "/hcs/v1/runtime");
    state.runtime = j;
    var l = $("loaded");
    if (j && j.loaded_model) { l.className = "pill pill-ok"; text(l, "loaded: " + j.loaded_model); }
    else { l.className = "pill pill-unknown"; text(l, "no model loaded"); }
    return j;
  }
  function trackJob(id, name, st) { state.jobs[id] = { name: name, state: st, updated: Date.now() }; }
  async function cancelJob(id) {
    try { var j = await api("POST", "/hcs/v1/jobs/" + encodeURIComponent(id) + "/cancel"); if (state.jobs[id]) state.jobs[id].state = (j && j.state) || "cancelled"; }
    catch (e) { setBanner("models-state", describe(e).kind, "cancel failed: " + describe(e).message); }
  }
  async function loadModel(id) {
    var slot = { jobId: "" };
    state.busy["load:" + id] = slot;
    setBanner("models-state", "loading", "starting load of " + id);
    renderModels();
    try {
      var j = await api("POST", "/hcs/v1/models/" + encodeURIComponent(id) + "/load");
      slot.jobId = j.job_id;
      trackJob(j.job_id, "load " + id, "queued");
      renderModels();
      var st = "queued";
      while (st === "queued" || st === "running") {
        await new Promise(function (r) { setTimeout(r, 500); });
        var s = await api("GET", "/hcs/v1/jobs/" + encodeURIComponent(j.job_id));
        st = s.state;
        trackJob(j.job_id, "load " + id, st);
        setBanner("models-state", "loading", id + ": " + st);
        refreshModels().catch(function () {});
        if (st === "failed") { var er = s.error || {}; var ae = new ApiError(er.code === "out_of_memory" ? 503 : 500, { error: er }, "load failed"); throw ae; }
      }
      if (st === "cancelled") setBanner("models-state", "idle");
      else setBanner("models-state", "ok", id + " loaded.");
    } catch (e) {
      var d = describe(e);
      setBanner("models-state", d.kind, d.message);
    } finally {
      delete state.busy["load:" + id];
      await refreshModels().catch(function () {});
    }
  }
  async function unloadModel(id) {
    setBanner("models-state", "busy", "unloading " + id);
    try { await api("POST", "/hcs/v1/models/" + encodeURIComponent(id) + "/unload"); setBanner("models-state", "ok", id + " unloaded."); }
    catch (e) { var d = describe(e); setBanner("models-state", d.kind, d.message); }
    await refreshModels().catch(function () {});
  }

  /* ---------- streaming chat (shared by Chat and Vision) ---------- */
  function selectedModel(id) { var v = $(id).value; return v ? v : undefined; }
  function jobFromChunkId(cid) { var m = /^chatcmpl-(\d+)$/.exec(cid || ""); return m ? "job-" + m[1] : ""; }
  /* Streams a chat completion. handlers: onJob(id), onDelta(text), onToolCall(index, id, name, argsDelta), onUsage(u). Returns {finish}. */
  async function streamChat(body, ctl, handlers) {
    body.stream = true;
    var r;
    try { r = await fetch("/v1/chat/completions", { method: "POST", headers: authHeaders({ "Content-Type": "application/json" }), body: JSON.stringify(body), signal: ctl.signal, cache: "no-store", credentials: "omit" }); }
    catch (e) { if (e && e.name === "AbortError") throw e; setConn(false, "server unreachable"); throw new ApiError(0, null, "server unreachable: " + e.message); }
    if (!r.ok) {
      var j = null, raw = "";
      try { raw = await r.text(); j = JSON.parse(raw); } catch (e) { j = null; }
      if (r.status === 401) needToken("The server rejected the token.");
      throw new ApiError(r.status, j, raw.slice(0, 200));
    }
    var reader = r.body.getReader(), dec = new TextDecoder(), buf = "", finish = "", gotJob = false;
    for (;;) {
      var chunk = await reader.read();
      if (chunk.done) break;
      buf += dec.decode(chunk.value, { stream: true });
      var idx;
      while ((idx = buf.indexOf("\n\n")) >= 0) {
        var ev = buf.slice(0, idx); buf = buf.slice(idx + 2);
        var line = ev.split("\n").filter(function (l) { return l.indexOf("data:") === 0; }).map(function (l) { return l.slice(5).trim(); }).join("");
        if (!line || line === "[DONE]") continue;
        var d; try { d = JSON.parse(line); } catch (e) { continue; }
        if (d.error) throw new ApiError(500, d, "stream aborted");
        if (!gotJob && d.id) { gotJob = true; handlers.onJob(jobFromChunkId(d.id)); }
        var ch = d.choices && d.choices[0];
        if (ch && ch.delta && ch.delta.content) handlers.onDelta(ch.delta.content);
        if (ch && ch.delta && Array.isArray(ch.delta.tool_calls) && handlers.onToolCall) {
          ch.delta.tool_calls.forEach(function (tc, k) {
            var f = tc.function || {};
            handlers.onToolCall(typeof tc.index === "number" ? tc.index : k, tc.id || "", f.name || "", f.arguments || "");
          });
        }
        if (ch && ch.finish_reason) finish = ch.finish_reason;
        if (d.usage && handlers.onUsage) handlers.onUsage(d.usage);
      }
    }
    return { finish: finish };
  }
  function numOrUndef(id) { var v = $(id).value; return v === "" ? undefined : Number(v); }

  /* ---------- Chat ---------- */
  var chat = { history: [], ctl: null, job: "" };
  function addMsg(role, content) {
    var d = el("div", "msg msg-" + role), head = el("div", "msghead");
    head.appendChild(el("b", "", role === "user" ? "You" : role === "assistant" ? "Model" : "Error"));
    var body = el("div", "msg-body");
    body.raw = content || "";
    if (role === "assistant") renderMd(body, body.raw); else text(body, body.raw);
    if (role !== "error") head.appendChild(copyButton(function () { return body.raw; }, role === "user" ? "message" : "reply", "Copy " + (role === "user" ? "your message" : "model reply")));
    d.appendChild(head);
    d.appendChild(body);
    var log = $("chat-log");
    $("chat-empty").hidden = true;
    log.appendChild(d);
    log.scrollTop = log.scrollHeight;
    return body;
  }
  var mdPending = null;
  function setReply(body, s) {            /* streamed replies re-render at most once per frame */
    body.raw = s;
    if (mdPending) return;
    mdPending = requestAnimationFrame(function () { mdPending = null; renderMd(body, body.raw); var l = $("chat-log"); l.scrollTop = l.scrollHeight; });
  }
  function finishReply(body) { if (mdPending) { cancelAnimationFrame(mdPending); mdPending = null; } renderMd(body, body.raw); }
  /* A model-requested tool call is shown, never executed (the Agent tab is planned). */
  function toolCallView(container) {
    var calls = {};
    return function (index, id, name, argsDelta) {
      var c = calls[index];
      if (!c) {
        var det = el("details", "toolcall"), sum = el("summary", "", "tool call"), pre = el("pre", "", "");
        det.open = true; det.appendChild(sum); det.appendChild(pre); container.appendChild(det);
        c = calls[index] = { sum: sum, pre: pre, name: "", args: "" };
      }
      if (name) c.name = name;
      c.args += argsDelta;
      text(c.sum, "tool call: " + (c.name || "...") + " (shown only, not executed)");
      var pretty = c.args;
      try { pretty = JSON.stringify(JSON.parse(c.args), null, 2); } catch (e) { /* partial JSON while streaming */ }
      text(c.pre, pretty || "(no arguments)");
    };
  }
  function setChatBusy(b) { $("chat-send").disabled = b; $("chat-stop").hidden = !b; $("chat-input").disabled = b; $("chat-log").setAttribute("aria-busy", b ? "true" : "false"); }
  async function sendChat() {
    var input = $("chat-input"), msg = input.value.trim();
    if (!msg || chat.ctl) return;
    input.value = "";
    chat.history.push({ role: "user", content: msg });
    addMsg("user", msg);
    var out = addMsg("assistant", "");
    var messages = [];
    var sys = $("chat-system").value.trim();
    if (state.mode === "advanced" && sys) messages.push({ role: "system", content: sys });
    messages = messages.concat(chat.history);
    var body = { messages: messages, model: selectedModel("chat-model") };
    if (state.mode === "advanced") {
      body.temperature = numOrUndef("chat-temp"); body.max_tokens = numOrUndef("chat-max"); body.seed = numOrUndef("chat-seed");
    }
    chat.ctl = new AbortController(); chat.job = "";
    setChatBusy(true);
    var willLoad = body.model && state.runtime && state.runtime.loaded_model !== body.model;
    setBanner("chat-state", willLoad ? "loading" : "busy", willLoad ? "loading " + body.model + " before generating (can take several seconds)" : "generating");
    var acc = "", t0 = performance.now(), first = 0, usage = null, onTool = toolCallView(out.parentNode);
    try {
      var res = await streamChat(body, chat.ctl, {
        onJob: function (id) { chat.job = id; if (id) trackJob(id, "chat", "running"); setBanner("chat-state", "busy", "generating"); },
        onDelta: function (t) { if (!first) first = performance.now() - t0; acc += t; setReply(out, acc); },
        onToolCall: onTool,
        onUsage: function (u) { usage = u; }
      });
      finishReply(out);
      chat.history.push({ role: "assistant", content: acc });
      if (acc && lstore("hcs.onboarded") !== "1") { lstore("hcs.onboarded", "1"); $("onboard").hidden = true; }
      if (chat.job) trackJob(chat.job, "chat", "succeeded");
      setBanner("chat-state", "idle");
      noteSpeed(usage, first, performance.now() - t0, "chat");
      text($("chat-meta"), (first ? "first token " + Math.round(first) + " ms" : "") + (usage ? " - " + (usage.completion_tokens || 0) + " tokens" : "") + (res.finish === "length" ? " - stopped at max tokens" : "") + (res.finish === "tool_calls" ? " - model requested a tool call" : ""));
    } catch (e) {
      var d = describe(e);
      finishReply(out);
      if (acc) chat.history.push({ role: "assistant", content: acc }); else chat.history.pop();
      if (chat.job) trackJob(chat.job, "chat", d.kind === "idle" ? "cancelled" : "failed");
      if (!acc && !out.parentNode.querySelector("details")) out.parentNode.parentNode.removeChild(out.parentNode);
      if (d.kind === "idle") setBanner("chat-state", "idle");
      else { setBanner("chat-state", d.kind, d.message); }
      if (d.kind === "idle") text($("chat-meta"), "stopped");
    } finally {
      chat.ctl = null; chat.job = "";
      setChatBusy(false);
      refreshRuntime().catch(function () {});
      $("chat-input").focus();
    }
  }
  async function stopChat() {
    if (!chat.ctl) return;
    var id = chat.job;
    if (id) await cancelJob(id);   /* explicit job cancel, then drop the connection (disconnect also cancels server-side) */
    if (chat.ctl) chat.ctl.abort();
  }

  /* ---------- Vision ---------- */
  var vision = { dataUri: "", ctl: null, job: "", url: "" };
  function setVisionBusy(b) { $("vision-send").disabled = b; $("vision-stop").hidden = !b; }
  function onVisionFile() {
    var f = $("vision-file").files[0], prev = $("vision-preview");
    vision.dataUri = "";
    if (vision.url) { URL.revokeObjectURL(vision.url); vision.url = ""; }
    prev.hidden = true;
    if (!f) return;
    if (!IMG_OK.test(f.type)) { setBanner("vision-state", "unsupported", "only PNG, JPEG, GIF or BMP images are accepted (" + (f.type || "unknown type") + " selected)."); return; }
    if (f.size > 32 * 1024 * 1024) { setBanner("vision-state", "error", "image is larger than the 32 MiB server limit."); return; }
    setBanner("vision-state", "idle");
    vision.url = URL.createObjectURL(f);
    prev.src = vision.url; prev.hidden = false;
    readFileAsDataUri(f, function (u) { vision.dataUri = u; }, function () { setBanner("vision-state", "error", "could not read the file."); });
  }
  async function sendVision(ev) {
    ev.preventDefault();
    if (vision.ctl) return;
    if (!vision.dataUri) { setBanner("vision-state", "error", "choose an image first."); return; }
    var prompt = $("vision-prompt").value.trim();
    if (!prompt) { setBanner("vision-state", "error", "enter a question about the image."); return; }
    var body = { model: selectedModel("vision-model"), messages: [{ role: "user", content: [{ type: "text", text: prompt }, { type: "image_url", image_url: { url: vision.dataUri } }] }] };
    if (state.mode === "advanced") { body.max_tokens = numOrUndef("vision-max"); body.temperature = numOrUndef("vision-temp"); }
    var out = $("vision-out"); out.raw = ""; renderMd(out, ""); $("vision-copy").hidden = true; out.setAttribute("aria-busy", "true");
    vision.ctl = new AbortController(); vision.job = "";
    setVisionBusy(true);
    var willLoad = body.model && state.runtime && state.runtime.loaded_model !== body.model;
    setBanner("vision-state", willLoad ? "loading" : "busy", willLoad ? "loading " + body.model + " with its projector" : "analyzing");
    var acc = "", usage = null, t0 = performance.now(), first = 0;
    try {
      await streamChat(body, vision.ctl, {
        onJob: function (id) { vision.job = id; if (id) trackJob(id, "vision", "running"); setBanner("vision-state", "busy", "analyzing"); },
        onDelta: function (t) { if (!first) first = performance.now() - t0; acc += t; setReply(out, acc); },
        onUsage: function (u) { usage = u; }
      });
      noteSpeed(usage, first, performance.now() - t0, "vision");
      if (vision.job) trackJob(vision.job, "vision", "succeeded");
      setBanner("vision-state", "idle");
    } catch (e) {
      var d = describe(e);
      if (vision.job) trackJob(vision.job, "vision", d.kind === "idle" ? "cancelled" : "failed");
      setBanner("vision-state", d.kind, d.message);
    } finally { finishReply(out); out.setAttribute("aria-busy", "false"); $("vision-copy").hidden = !out.raw; vision.ctl = null; vision.job = ""; setVisionBusy(false); refreshRuntime().catch(function () {}); }
  }
  async function stopVision() {
    if (!vision.ctl) return;
    if (vision.job) await cancelJob(vision.job);
    if (vision.ctl) vision.ctl.abort();
  }

  /* ---------- Image Studio ---------- */
  var image = { ctl: null, initUri: "", gallery: [], current: -1, timer: 0, blobUrl: "" };
  var GALLERY_MAX = 24;
  function imageMode() { var r = document.querySelector("input[name=image-mode]:checked"); return r ? r.value : "txt2img"; }
  function setImageMode(m) {
    Array.prototype.forEach.call(document.querySelectorAll("input[name=image-mode]"), function (r) { r.checked = r.value === m; });
    var i2i = m === "img2img";
    $("image-init-box").hidden = !i2i;
    var match = $("image-size-match");
    match.hidden = !i2i; match.disabled = !i2i;
    if (i2i && image.initUri) $("image-size").value = "";
    else if (!i2i && $("image-size").value === "") $("image-size").value = "512x512";
  }
  function syncPreset() {
    var v = $("image-steps").value;
    Array.prototype.forEach.call(document.querySelectorAll("input[name=image-preset]"), function (r) { r.checked = r.value === v; });
  }
  function setInitImage(uri, label) {
    image.initUri = uri;
    var prev = $("image-init-preview");
    if (!uri) { prev.hidden = true; text($("image-drop-text"), "Drop an image here or choose one (PNG, JPEG, GIF, BMP)"); return; }
    prev.src = uri; prev.hidden = false;
    text($("image-drop-text"), label || "Input image ready. Drop another to replace it.");
    if (imageMode() === "img2img") $("image-size").value = "";
  }
  function onImageFile(f) {
    if (!f) return;
    if (!IMG_OK.test(f.type)) { setBanner("image-state", "unsupported", "only PNG, JPEG, GIF or BMP images are accepted (" + (f.type || "unknown type") + " selected)."); return; }
    if (f.size > 32 * 1024 * 1024) { setBanner("image-state", "error", "image is larger than the 32 MiB limit."); return; }
    setBanner("image-state", "idle");
    readFileAsDataUri(f, function (u) { setInitImage(u, f.name + " loaded. Drop another to replace it."); }, function () { setBanner("image-state", "error", "could not read the file."); });
  }
  function showGalleryItem(i) {
    var g = image.gallery[i];
    if (!g) return;
    image.current = i;
    $("image-out").src = g.src; $("image-out").hidden = false; $("image-empty").hidden = true;
    text($("image-meta"), g.meta + " - prompt: " + g.prompt);
    $("image-actions").hidden = false;
    renderGallery();
  }
  function renderGallery() {
    var ul = $("image-gallery");
    clear(ul);
    image.gallery.forEach(function (g, i) {
      var li = el("li"), b = el("button", "ghost");
      b.type = "button";
      b.setAttribute("aria-label", "Image " + (i + 1) + ": " + g.meta);
      b.title = g.prompt + " - " + g.meta;
      if (i === image.current) b.setAttribute("aria-current", "true");
      var im = el("img"); im.src = g.src; im.alt = "";
      b.appendChild(im);
      b.addEventListener("click", function () { showGalleryItem(i); });
      li.appendChild(b); ul.appendChild(li);
    });
  }
  /* LoRA library (GET /hcs/v1/loras): ids and metadata only, never paths. */
  var loraLib = [];
  function renderLoras() {
    var ul = $("image-lora-list");
    clear(ul);
    loraLib.forEach(function (l) {
      var li = el("li", "lora-item"), lab = el("label", "check"), cb = el("input");
      cb.type = "checkbox"; cb.dataset.loraId = l.id; cb.disabled = !l.usable;
      lab.appendChild(cb);
      var words = (l.trigger_words && l.trigger_words.length) ? " - trigger: " + l.trigger_words.join(", ") : "";
      lab.appendChild(document.createTextNode(" " + (l.name || l.id) + " (" + l.base_family + ", " + (l.licence || "licence unknown") + ")" + words + (l.usable ? "" : " - " + l.status + (l.problems && l.problems.length ? ": " + l.problems.join("; ") : ""))));
      li.appendChild(lab);
      var w = el("input"); w.type = "number"; w.min = "-2"; w.max = "2"; w.step = "0.05"; w.value = String(l.recommended_weight); w.disabled = !l.usable;
      w.id = "image-lora-w-" + l.id; w.setAttribute("aria-label", "Weight of " + (l.name || l.id));
      li.appendChild(w);
      ul.appendChild(li);
    });
    text($("image-lora-note"), loraLib.length ? "" : "No LoRAs found. Put files and a manifest.json in <model root>/lora/image (see docs/lora.md).");
  }
  async function refreshLoras() {
    try {
      var j = await api("GET", "/hcs/v1/loras");
      loraLib = (j && j.loras) || [];
    } catch (e) { loraLib = []; }
    renderLoras();
  }
  function selectedLoras() {
    var out = [];
    Array.prototype.forEach.call(document.querySelectorAll("#image-lora-list input[type=checkbox]"), function (cb) {
      if (!cb.checked) return;
      var w = Number($("image-lora-w-" + cb.dataset.loraId).value);
      out.push({ id: cb.dataset.loraId, weight: w });
    });
    return out;
  }
  function clearLoras() {
    Array.prototype.forEach.call(document.querySelectorAll("#image-lora-list input[type=checkbox]"), function (cb) { cb.checked = false; });
  }
  function stopImageTimer() { if (image.timer) { clearInterval(image.timer); image.timer = 0; } }
  async function sendImage(ev) {
    ev.preventDefault();
    if (image.ctl) return;
    var prompt = $("image-prompt").value.trim();
    if (!prompt) { setBanner("image-state", "error", "enter a prompt."); return; }
    var i2i = imageMode() === "img2img";
    if (i2i && !image.initUri) { setBanner("image-state", "error", "choose an input image for image to image."); return; }
    var seed = numOrUndef("image-seed");
    if (seed === undefined) seed = randomSeed();
    var body = { model: selectedModel("image-model"), prompt: prompt, n: 1, response_format: "b64_json", seed: seed };
    var size = $("image-size").value;
    if (size) body.size = size;
    var steps = numOrUndef("image-steps");
    if (steps !== undefined) body.steps = steps;
    var mp = $("image-modelpreset").value;
    if (mp) body.preset = mp;
    if (i2i) { body.image = image.initUri; body.strength = Number($("image-strength").value); }
    if (state.mode === "advanced") {
      var neg = $("image-negative").value.trim(); if (neg) body.negative_prompt = neg;
      var cfg = numOrUndef("image-cfg"); if (cfg !== undefined) body.cfg_scale = cfg;
      var smp = $("image-sampler").value; if (smp) body.sampler = smp;
      if ($("image-lowq").checked) body.allow_low_quality = true;
      var picked = selectedLoras();
      if (picked.length > 4) { setBanner("image-state", "error", "at most 4 LoRAs per image."); return; }
      if (picked.length) body.loras = picked;
    }
    if ($("image-upscale").value === "4k") body.upscale = "4k";
    image.ctl = new AbortController();
    $("image-send").disabled = true; $("image-stop").hidden = false;
    $("image-busy").hidden = false; $("image-empty").hidden = true;
    var t0 = performance.now();
    setBanner("image-state", "busy", (i2i ? "editing" : "generating") + " (the server reports no per-step progress; Cancel drops the request and stops the render)");
    stopImageTimer();
    image.timer = setInterval(function () { text($("image-meta"), "Working... " + Math.round((performance.now() - t0) / 1000) + " s elapsed"); }, 500);
    try {
      var j = await api("POST", i2i ? "/v1/images/edits" : "/v1/images/generations", body, image.ctl.signal);
      var item = j && j.data && j.data[0];
      var src = item && (item.b64_json ? "data:image/png;base64," + item.b64_json : (item.url && item.url.indexOf("data:image/") === 0 ? item.url : ""));
      if (!src) throw new ApiError(502, null, "the server returned no image data");
      var h = (j && j.hcs) || {};
      var meta = (i2i ? "image to image" : "text to image") + " - " + (h.width || "?") + "x" + (h.height || "?") + ", " + (h.steps || steps || "?") + " steps, seed " + seed + (typeof h.generation_ms === "number" ? ", " + (h.generation_ms / 1000).toFixed(1) + " s" : ", " + ((performance.now() - t0) / 1000).toFixed(1) + " s wall") + (h.model ? " - " + h.model : "");
      var note = "";
      if (h.loras && h.loras.length) meta += " - LoRA " + h.loras.join(", ");
      if (h.upscale) {
        if (h.upscale.status === "done") meta += " - upscaled from " + h.upscale.from_width + "x" + h.upscale.from_height + " (" + h.upscale.provider + ", " + (h.upscale.ms / 1000).toFixed(1) + " s)";
        else if (h.upscale.status === "skipped") note = " Upscale skipped: " + h.upscale.note;
        else note = " Upscale failed, the original image is shown: " + h.upscale.error;
      }
      image.gallery.push({ src: src, meta: meta, seed: seed, prompt: prompt });
      if (image.gallery.length > GALLERY_MAX) image.gallery.shift();
      showGalleryItem(image.gallery.length - 1);
      setBanner("image-state", "ok", "image generated." + note);
    } catch (e) {
      var d = describe(e);
      text($("image-meta"), "");
      setBanner("image-state", d.kind, d.message);
      if (!image.gallery.length) $("image-empty").hidden = false;
    } finally {
      stopImageTimer();
      image.ctl = null; $("image-send").disabled = false; $("image-stop").hidden = true; $("image-busy").hidden = true;
      refreshRuntime().catch(function () {});
    }
  }
  /* Upscale the shown gallery image (POST /hcs/v1/images/upscale). Measured on the CPU (docs/upscale.md); shown
     to the user BEFORE the long wait so it is never a surprise. */
  var UPSCALE_HINT = "UPSCALE_HINT_PLACEHOLDER";
  async function upscaleCurrentImage() {
    var g = image.gallery[image.current];
    if (!g || image.ctl) return;
    image.ctl = new AbortController();
    $("image-send").disabled = true; $("image-upscale-now").disabled = true; $("image-stop").hidden = false;
    $("image-busy").hidden = false;
    var t0 = performance.now();
    setBanner("image-state", "busy", "upscaling to 4K. " + UPSCALE_HINT + " Cancel stops it between tiles.");
    stopImageTimer();
    image.timer = setInterval(function () { text($("image-meta"), "Upscaling... " + Math.round((performance.now() - t0) / 1000) + " s elapsed"); }, 500);
    try {
      var j = await api("POST", "/hcs/v1/images/upscale", { image: g.src, upscale: "4k" }, image.ctl.signal);
      var item = j && j.data && j.data[0];
      if (!item || !item.b64_json) throw new ApiError(502, null, "the server returned no image data");
      var h = (j && j.hcs) || {}, up = h.upscale || {};
      if (up.status === "skipped") {
        setBanner("image-state", "ok", "nothing to do: " + (up.note || "the image is already at or above 3840 px."));
        showGalleryItem(image.current);
        return;
      }
      var meta = "upscaled 4K - " + (h.width || "?") + "x" + (h.height || "?") + " from " + (up.from_width || "?") + "x" + (up.from_height || "?") + " (" + (up.provider || "?") + ", " + (typeof up.ms === "number" ? (up.ms / 1000).toFixed(1) + " s" : "?") + (up.tiles ? ", " + up.tiles + " tiles of " + up.tile + " px" : "") + ")";
      image.gallery.push({ src: "data:image/png;base64," + item.b64_json, meta: meta, seed: g.seed, prompt: g.prompt });
      if (image.gallery.length > GALLERY_MAX) image.gallery.shift();
      showGalleryItem(image.gallery.length - 1);
      setBanner("image-state", "ok", "image upscaled.");
    } catch (e) {
      var d = describe(e);
      setBanner("image-state", d.kind, d.message);
      showGalleryItem(image.current);
    } finally {
      stopImageTimer();
      image.ctl = null; $("image-send").disabled = false; $("image-upscale-now").disabled = false; $("image-stop").hidden = true; $("image-busy").hidden = true;
    }
  }
  function saveCurrentImage() {
    var g = image.gallery[image.current];
    if (!g) return;
    var blob = dataUriToBlob(g.src);
    if (!blob) { setBanner("image-state", "error", "could not prepare the file."); return; }
    if (image.blobUrl) URL.revokeObjectURL(image.blobUrl);
    image.blobUrl = URL.createObjectURL(blob);
    var a = document.createElement("a");
    a.href = image.blobUrl; a.download = "hcs-studio-seed-" + g.seed + ".png";
    document.body.appendChild(a); a.click(); document.body.removeChild(a);
  }

  /* ---------- Performance ---------- */
  var speed = [];   /* measured in this page: {kind, tokens, ttft, tps} */
  function noteSpeed(usage, firstMs, totalMs, kind) {
    var n = usage && typeof usage.completion_tokens === "number" ? usage.completion_tokens : 0;
    var win = totalMs - (firstMs || 0);
    if (n < 2 || win <= 0) return;
    speed.push({ kind: kind, tokens: n, ttft: firstMs || 0, tps: n / (win / 1000) });
    if (speed.length > 10) speed.shift();
  }
  function renderSpeed(rt) {
    var last = speed[speed.length - 1], hist = $("perf-speed-hist");
    clear(hist);
    var em = rt && rt.text_engine && rt.text_engine.metrics;
    if (last) {
      text($("perf-tps"), last.tps.toFixed(1));
      text($("perf-speed-note"), "Last " + last.kind + " run: " + last.tokens + " tokens, first token after " + Math.round(last.ttft) + " ms (measured in this page)." + (em && em.backend && em.backend !== "unknown" ? " Backend: " + em.backend + (em.device && em.device !== "unknown" ? " on " + em.device : "") + "." : ""));
      var peak = Math.max.apply(null, speed.map(function (x) { return x.tps; }));
      speed.forEach(function (x, i) {
        var li = el("li"), b = el("span"); b.style.height = Math.max(6, Math.round(100 * x.tps / peak)) + "%";
        li.title = x.kind + " " + (i + 1) + ": " + x.tps.toFixed(1) + " tokens/s"; li.appendChild(b); hist.appendChild(li);
      });
    } else if (em && typeof em.tokens_per_second === "number" && em.tokens_per_second > 0) {
      text($("perf-tps"), em.tokens_per_second.toFixed(1));
      text($("perf-speed-note"), "Engine-reported speed of the last run" + (em.ttft_ms ? ", first token after " + Math.round(em.ttft_ms) + " ms" : "") + ".");
    } else {
      text($("perf-tps"), "-");
      text($("perf-speed-note"), "No generation from this page yet. Send a chat message and the measured speed appears here.");
    }
  }
  function mib(n) { return typeof n === "number" ? fmtBytes(n) : "unknown"; }
  function renderDevices(dev) {
    var box = $("perf-dev"); clear(box);
    if (!dev) return;
    var cpu = dev.cpu;
    if (cpu) {
      var f = cpu.features || {}, flags = ["avx2", "fma3", "avx512f"].filter(function (k) { return f[k]; }).map(function (k) { return k.toUpperCase(); });
      var c = el("div", "dev");
      c.appendChild(el("b", "", "CPU"));
      c.appendChild(el("span", "", (cpu.name || "unknown") + " - " + (cpu.physical_cores || "?") + " cores / " + (cpu.logical_processors || "?") + " threads" + (flags.length ? " - " + flags.join(" ") : "")));
      box.appendChild(c);
    }
    var seen = {};
    ((dev.gpu && dev.gpu.adapters) || []).forEach(function (g) {
      var key = (g.name || "") + "|" + (g.device_id || "") + "|" + (g.software_adapter ? "sw" : "hw");
      if (seen[key]) return;
      seen[key] = true;
      var d = el("div", "dev"), loc = g.memory_local_segment || {};
      d.appendChild(el("b", "", g.software_adapter ? "Software renderer" : "GPU"));
      d.appendChild(el("span", "", (g.name || "unknown") + (g.software_adapter ? " (not used for inference)" : " - " + mib(g.dedicated_video_memory_bytes) + " reserved, " + mib(g.shared_system_memory_bytes) + " shared system memory" + (typeof loc.budget_bytes === "number" && loc.budget_bytes ? ", OS budget " + mib(loc.budget_bytes) : ""))));
      box.appendChild(d);
    });
    if (!box.firstChild) box.appendChild(el("p", "note", "The server returned no device list."));
  }
  async function refreshPerf() {
    try {
      var rt = await api("GET", "/hcs/v1/runtime");
      state.runtime = rt;
      var mem = await api("GET", "/hcs/v1/memory");
      var dev = await api("GET", "/hcs/v1/devices");
      var a = $("perf-runtime"), b = $("perf-memory"), c = $("perf-jobs");
      clear(a); clear(b); clear(c);
      function row(dl, k, v) { dl.appendChild(el("dt", "", k)); dl.appendChild(el("dd", "", v)); }
      var te = rt.text_engine || {}, ie = rt.image_engine || {}, tm = te.metrics || {}, im = ie.metrics || {};
      row(a, "Version", rt.version || "unknown");
      row(a, "Uptime", typeof rt.uptime_s === "number" ? Math.round(rt.uptime_s) + " s" : "unknown");
      row(a, "Loaded model", rt.loaded_model || "none");
      row(a, "Jobs pending", String(rt.jobs_pending == null ? "unknown" : rt.jobs_pending));
      row(a, "Text engine", (te.available ? "available" : "unavailable") + (tm.backend && tm.backend !== "unknown" ? ", " + tm.backend : "") + (tm.load_ms ? ", loaded in " + Math.round(tm.load_ms) + " ms" : ""));
      row(a, "Image engine", (ie.available ? "available" : "unavailable") + (im.backend && im.backend !== "unknown" ? ", " + im.backend : "") + (im.image_generation_ms ? ", last image " + (im.image_generation_ms / 1000).toFixed(1) + " s" : ""));
      row(a, "Chat template", rt.chat_template || "unknown");
      [["Budget", mem.total_bytes], ["Reserved", mem.reserved_bytes], ["Remaining", mem.remaining_bytes]].forEach(function (x) { if (typeof x[1] === "number") row(b, x[0], fmtBytes(x[1])); });
      var ms = mem.measured || {};
      [["Available now", ms.available_physical_bytes], ["OS reserve", ms.os_reserve_bytes], ["Safety margin", ms.safety_margin_bytes]].forEach(function (x) { if (typeof x[1] === "number") row(b, x[0], fmtBytes(x[1])); });
      row(b, "Vulkan headroom", typeof ms.vulkan_headroom_bytes === "number" ? fmtBytes(ms.vulkan_headroom_bytes) : "unknown on this build");
      renderMemory(mem, "perf-mem");
      renderSpeed(rt);
      Object.keys(state.jobs).slice(-12).forEach(function (id) { c.appendChild(el("dt", "", id + " (" + state.jobs[id].name + ")")); c.appendChild(el("dd", "", state.jobs[id].state)); });
      if (!Object.keys(state.jobs).length) { c.appendChild(el("dt", "", "none")); c.appendChild(el("dd", "", "")); }
      renderDevices(dev);
      text($("perf-devices"), JSON.stringify(dev, null, 2));
      setBanner("perf-state", "idle");
      setConn(true, "connected");
    } catch (e) { var d = describe(e); setBanner("perf-state", d.kind, d.message); }
  }
  function startPerf() { stopPerf(); refreshPerf(); var s = Math.max(1, Math.min(60, Number($("set-interval").value) || DEFAULTS.interval)); state.perfTimer = setInterval(function () { if ($("perf-auto").checked && !document.hidden) refreshPerf(); }, s * 1000); }
  function stopPerf() { if (state.perfTimer) { clearInterval(state.perfTimer); state.perfTimer = 0; } }

  /* ---------- first-run guide ---------- */
  async function renderOnboard() {
    var box = $("onboard");
    if (lstore("hcs.onboarded") === "1") { box.hidden = true; return; }
    box.hidden = false;
    var real = state.models.filter(function (m) { return !/^mmproj/i.test(m.id); });
    text($("ob-models"), real.length ? real.length + " model" + (real.length === 1 ? "" : "s") + " registered on the server." : "No models registered yet.");
    if (state.obSys) { text($("ob-system"), state.obSys); }
    else try {
      var dev = await api("GET", "/hcs/v1/devices"), mem = await api("GET", "/hcs/v1/memory"), parts = [];
      if (dev.cpu) parts.push((dev.cpu.name || "CPU") + ", " + (dev.cpu.physical_cores || "?") + " cores");
      var gpus = ((dev.gpu && dev.gpu.adapters) || []).filter(function (g) { return !g.software_adapter; });
      parts.push(gpus.length ? "GPU: " + gpus[0].name : "no GPU adapter reported (CPU only)");
      if (mem.measured && typeof mem.measured.available_physical_bytes === "number") parts.push(fmtBytes(mem.measured.available_physical_bytes) + " memory available");
      state.obSys = parts.join(" - ") + ".";
      text($("ob-system"), state.obSys);
    } catch (e) { text($("ob-system"), "Could not read the device report: " + describe(e).message); }
    Array.prototype.forEach.call(document.querySelectorAll("input[name=ob-mode]"), function (r) { r.checked = r.value === state.mode; });
  }
  function resetUi() {
    ["hcs.theme", "hcs.mode", "hcs.onboarded"].forEach(function (k) { try { localStorage.removeItem(k); } catch (e) { /* ignore */ } });
    $("set-interval").value = String(DEFAULTS.interval);
    setTheme("system"); setMode("simple"); renderOnboard();
    toast("UI settings reset.");
  }

  /* ---------- advanced reset ---------- */
  var RESETS = {
    chat: { "chat-system": "", "chat-temp": "0.7", "chat-max": "256", "chat-seed": "" },
    vision: { "vision-max": "256", "vision-temp": "0" },
    image: { "image-negative": "", "image-cfg": "", "image-sampler": "", "image-modelpreset": "", "image-upscale": "off" }
  };

  /* ---------- wiring ---------- */
  function init() {
    if (location.search && /token=/i.test(location.search)) {
      /* A token in a query string would leak into logs and history; never read it. */
      try { history.replaceState(null, "", location.pathname + location.hash); } catch (e) { /* ignore */ }
    }
    takeTokenFromFragment();
    text($("set-origin"), location.origin);
    initTabs();
    setMode(lstore("hcs.mode") || "simple");
    setTheme(lstore("hcs.theme") || "system");
    Array.prototype.forEach.call(document.querySelectorAll("input[name=mode]"), function (r) { r.addEventListener("change", function () { if (r.checked) setMode(r.value); }); });
    Array.prototype.forEach.call(document.querySelectorAll("input[name=theme]"), function (r) { r.addEventListener("change", function () { if (r.checked) setTheme(r.value); }); });
    $("theme-toggle").addEventListener("click", function () { setTheme(THEMES[(THEMES.indexOf(state.theme) + 1) % THEMES.length]); });
    Array.prototype.forEach.call(document.querySelectorAll("[data-reset]"), function (b) {
      b.addEventListener("click", function () { var m = RESETS[b.dataset.reset]; Object.keys(m).forEach(function (k) { $(k).value = m[k]; }); if (b.dataset.reset === "image") clearLoras(); });
    });
    $("auth-form").addEventListener("submit", function (e) {
      e.preventDefault();
      state.token = $("auth-token").value.trim(); $("auth-token").value = "";
      store("hcs.token", state.token);
      connect();
    });
    $("set-forget").addEventListener("click", function () { state.token = ""; store("hcs.token", null); stopPerf(); needToken("Token forgotten."); });
    $("set-interval").addEventListener("change", function () { if (!$("panel-perf").hidden) startPerf(); });
    $("set-interval-reset").addEventListener("click", function () { $("set-interval").value = String(DEFAULTS.interval); });
    $("chat-form").addEventListener("submit", function (e) { e.preventDefault(); sendChat(); });
    $("chat-input").addEventListener("keydown", function (e) { if (e.key === "Enter" && !e.shiftKey && !e.isComposing) { e.preventDefault(); sendChat(); } });
    $("chat-stop").addEventListener("click", stopChat);
    $("chat-clear").addEventListener("click", function () {
      if (chat.ctl) return;
      chat.history = [];
      var log = $("chat-log");
      Array.prototype.slice.call(log.querySelectorAll(".msg")).forEach(function (n) { log.removeChild(n); });
      $("chat-empty").hidden = false;
      text($("chat-meta"), ""); setBanner("chat-state", "idle");
    });
    $("vision-file").addEventListener("change", onVisionFile);
    $("vision-form").addEventListener("submit", sendVision);
    $("vision-stop").addEventListener("click", stopVision);
    /* Image Studio */
    $("image-form").addEventListener("submit", sendImage);
    $("image-stop").addEventListener("click", function () { if (image.ctl) image.ctl.abort(); });
    Array.prototype.forEach.call(document.querySelectorAll("input[name=image-mode]"), function (r) { r.addEventListener("change", function () { if (r.checked) setImageMode(r.value); }); });
    Array.prototype.forEach.call(document.querySelectorAll("input[name=image-preset]"), function (r) { r.addEventListener("change", function () { if (r.checked) $("image-steps").value = r.value; }); });
    $("image-steps").addEventListener("input", syncPreset);
    $("image-dice").addEventListener("click", function () { $("image-seed").value = String(randomSeed()); });
    $("image-strength").addEventListener("input", function () { text($("image-strength-val"), Number($("image-strength").value).toFixed(2)); });
    $("image-file").addEventListener("change", function () { onImageFile($("image-file").files[0]); });
    var drop = $("image-drop");
    ["dragenter", "dragover"].forEach(function (n) { drop.addEventListener(n, function (e) { e.preventDefault(); drop.classList.add("over"); }); });
    ["dragleave", "drop"].forEach(function (n) { drop.addEventListener(n, function (e) { e.preventDefault(); drop.classList.remove("over"); }); });
    drop.addEventListener("drop", function (e) { var f = e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files[0]; onImageFile(f); });
    $("image-save").addEventListener("click", saveCurrentImage);
    $("image-upscale-now").addEventListener("click", upscaleCurrentImage);
    $("image-reuse").addEventListener("click", function () {
      var g = image.gallery[image.current];
      if (!g) return;
      setInitImage(g.src, "Previous result is the input image.");
      setImageMode("img2img");
    });
    $("image-copy-prompt").addEventListener("click", function () { var g = image.gallery[image.current]; if (g) copyText(g.prompt, "Prompt"); });
    $("image-copy-seed").addEventListener("click", function () {
      var g = image.gallery[image.current];
      if (!g) return;
      $("image-seed").value = String(g.seed);
      copyText(String(g.seed), "Seed");
      setBanner("image-state", "ok", "seed " + g.seed + " is now in the Seed field.");
    });
    $("image-steps").value = "20"; syncPreset();
    setImageMode("txt2img");
    $("vision-copy").addEventListener("click", function () { copyText($("vision-out").raw || "", "Answer"); });
    $("ob-copy").addEventListener("click", function () { copyText("HCSInfer models scan <folder>", "Command"); });
    $("ob-open-models").addEventListener("click", function () { showPanel("models"); $("tab-models").focus(); });
    $("ob-done").addEventListener("click", function () { lstore("hcs.onboarded", "1"); $("onboard").hidden = true; toast("Guide hidden. It is in Settings if you want it back."); });
    Array.prototype.forEach.call(document.querySelectorAll("input[name=ob-mode]"), function (r) { r.addEventListener("change", function () { if (r.checked) setMode(r.value); }); });
    $("set-guide").addEventListener("click", function () { try { localStorage.removeItem("hcs.onboarded"); } catch (e) { /* ignore */ } renderOnboard(); showPanel("chat"); });
    $("set-reset").addEventListener("click", resetUi);
    $("models-refresh").addEventListener("click", function () { refreshModels().catch(function (e) { var d = describe(e); setBanner("models-state", d.kind, d.message); }); });
    $("perf-refresh").addEventListener("click", refreshPerf);
    connect();
  }
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init); else init();
})();
