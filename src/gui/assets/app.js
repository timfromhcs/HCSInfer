/* HCSInfer local UI. Vanilla JS, no dependencies, no inline handlers, no network access except same-origin API calls.
   Everything shown comes from the server; unsupported features display the server's reason. */
"use strict";
(function () {
  var $ = function (id) { return document.getElementById(id); };
  var DEFAULTS = { interval: 3 };
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

  /* ---------- tabs / mode ---------- */
  var panels = ["chat", "vision", "image", "models", "perf", "settings"];
  function showPanel(name) {
    panels.forEach(function (p) {
      var on = p === name;
      $("panel-" + p).hidden = !on;
      var t = $("tab-" + p);
      t.setAttribute("aria-selected", on ? "true" : "false");
      t.tabIndex = on ? 0 : -1;
    });
    if (name === "perf") startPerf(); else stopPerf();
    if (name === "models") refreshModels().catch(function () {});
  }
  function initTabs() {
    var tabs = $("tabs");
    tabs.addEventListener("click", function (e) { var b = e.target.closest("[role=tab]"); if (b) showPanel(b.dataset.panel); });
    tabs.addEventListener("keydown", function (e) {
      var i = panels.indexOf(document.activeElement && document.activeElement.dataset ? document.activeElement.dataset.panel : "");
      if (i < 0) return;
      var n = e.key === "ArrowRight" ? i + 1 : e.key === "ArrowLeft" ? i - 1 : e.key === "Home" ? 0 : e.key === "End" ? panels.length - 1 : -1;
      if (n < 0) return;
      e.preventDefault();
      n = (n + panels.length) % panels.length;
      showPanel(panels[n]);
      $("tab-" + panels[n]).focus();
    });
  }
  function setMode(m) {
    state.mode = m === "advanced" ? "advanced" : "simple";
    lstore("hcs.mode", state.mode);
    Array.prototype.forEach.call(document.querySelectorAll("[data-advanced]"), function (n) { n.hidden = state.mode !== "advanced"; });
    Array.prototype.forEach.call(document.querySelectorAll("input[name=mode]"), function (r) { r.checked = r.value === state.mode; });
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
      tr.appendChild(el("td", "", m.display_name && m.display_name !== m.id ? m.display_name + " (" + m.id + ")" : m.id));
      tr.appendChild(el("td", "", kindOf(m)));
      tr.appendChild(el("td", "", m.quantization || "unknown"));
      tr.appendChild(el("td", "", fmtBytes(m.size_bytes)));
      tr.appendChild(el("td", "", m.state));
      var act = el("td");
      var loading = state.busy["load:" + m.id];
      var isLoaded = m.state === "loaded" || m.state === "running";
      var lb = el("button", "", loading ? "Loading..." : "Load");
      lb.disabled = !!loading || isLoaded;
      lb.addEventListener("click", function () { loadModel(m.id); });
      act.appendChild(lb);
      if (loading && loading.jobId) {
        var cb = el("button", "danger", "Cancel");
        cb.addEventListener("click", function () { cancelJob(loading.jobId); });
        act.appendChild(document.createTextNode(" "));
        act.appendChild(cb);
      }
      var ub = el("button", "ghost", "Unload");
      ub.disabled = !isLoaded || !!loading;
      ub.addEventListener("click", function () { unloadModel(m.id); });
      act.appendChild(document.createTextNode(" "));
      act.appendChild(ub);
      tr.appendChild(act);
      tb.appendChild(tr);
    });
  }
  async function refreshModels() {
    var j = await api("GET", "/hcs/v1/models");
    state.models = (j && j.models) || [];
    await refreshRuntime().catch(function () {});
    renderModels();
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
  /* Streams a chat completion. handlers: onJob(id), onDelta(text), onUsage(u). Returns {finish, aborted}. */
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
    var d = el("div", "msg msg-" + role);
    d.appendChild(el("b", "", role === "user" ? "You" : role === "assistant" ? "Model" : "Error"));
    var body = el("span", "", content);
    d.appendChild(body);
    var log = $("chat-log");
    log.appendChild(d);
    log.scrollTop = log.scrollHeight;
    return body;
  }
  function setChatBusy(b) { $("chat-send").disabled = b; $("chat-stop").hidden = !b; $("chat-input").disabled = b; }
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
    var acc = "", t0 = performance.now(), first = 0, usage = null;
    try {
      var res = await streamChat(body, chat.ctl, {
        onJob: function (id) { chat.job = id; if (id) trackJob(id, "chat", "running"); setBanner("chat-state", "busy", "generating"); },
        onDelta: function (t) { if (!first) first = performance.now() - t0; acc += t; text(out, acc); var l = $("chat-log"); l.scrollTop = l.scrollHeight; },
        onUsage: function (u) { usage = u; }
      });
      chat.history.push({ role: "assistant", content: acc });
      if (chat.job) trackJob(chat.job, "chat", "succeeded");
      setBanner("chat-state", "idle");
      text($("chat-meta"), (first ? "first token " + Math.round(first) + " ms" : "") + (usage ? " - " + (usage.completion_tokens || 0) + " tokens" : "") + (res.finish === "length" ? " - stopped at max tokens" : ""));
    } catch (e) {
      var d = describe(e);
      if (acc) chat.history.push({ role: "assistant", content: acc }); else chat.history.pop();
      if (chat.job) trackJob(chat.job, "chat", d.kind === "idle" ? "cancelled" : "failed");
      if (!acc) out.parentNode.parentNode.removeChild(out.parentNode);
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
    if (!/^image\/(png|jpeg|gif|bmp)$/.test(f.type)) { setBanner("vision-state", "unsupported", "only PNG, JPEG, GIF or BMP images are accepted (" + (f.type || "unknown type") + " selected)."); return; }
    if (f.size > 32 * 1024 * 1024) { setBanner("vision-state", "error", "image is larger than the 32 MiB server limit."); return; }
    setBanner("vision-state", "idle");
    vision.url = URL.createObjectURL(f);
    prev.src = vision.url; prev.hidden = false;
    var fr = new FileReader();
    fr.onload = function () { vision.dataUri = String(fr.result); };
    fr.onerror = function () { setBanner("vision-state", "error", "could not read the file."); };
    fr.readAsDataURL(f);
  }
  async function sendVision(ev) {
    ev.preventDefault();
    if (vision.ctl) return;
    if (!vision.dataUri) { setBanner("vision-state", "error", "choose an image first."); return; }
    var prompt = $("vision-prompt").value.trim();
    if (!prompt) { setBanner("vision-state", "error", "enter a question about the image."); return; }
    var body = { model: selectedModel("vision-model"), messages: [{ role: "user", content: [{ type: "text", text: prompt }, { type: "image_url", image_url: { url: vision.dataUri } }] }] };
    if (state.mode === "advanced") { body.max_tokens = numOrUndef("vision-max"); body.temperature = numOrUndef("vision-temp"); }
    var out = $("vision-out"); text(out, "");
    vision.ctl = new AbortController(); vision.job = "";
    setVisionBusy(true);
    var willLoad = body.model && state.runtime && state.runtime.loaded_model !== body.model;
    setBanner("vision-state", willLoad ? "loading" : "busy", willLoad ? "loading " + body.model + " with its projector" : "analyzing");
    var acc = "";
    try {
      await streamChat(body, vision.ctl, {
        onJob: function (id) { vision.job = id; if (id) trackJob(id, "vision", "running"); setBanner("vision-state", "busy", "analyzing"); },
        onDelta: function (t) { acc += t; text(out, acc); }
      });
      if (vision.job) trackJob(vision.job, "vision", "succeeded");
      setBanner("vision-state", "idle");
    } catch (e) {
      var d = describe(e);
      if (vision.job) trackJob(vision.job, "vision", d.kind === "idle" ? "cancelled" : "failed");
      setBanner("vision-state", d.kind, d.message);
    } finally { vision.ctl = null; vision.job = ""; setVisionBusy(false); refreshRuntime().catch(function () {}); }
  }
  async function stopVision() {
    if (!vision.ctl) return;
    if (vision.job) await cancelJob(vision.job);
    if (vision.ctl) vision.ctl.abort();
  }

  /* ---------- Image Studio ---------- */
  var image = { ctl: null };
  async function sendImage(ev) {
    ev.preventDefault();
    if (image.ctl) return;
    var prompt = $("image-prompt").value.trim();
    if (!prompt) { setBanner("image-state", "error", "enter a prompt."); return; }
    var body = { model: selectedModel("image-model"), prompt: prompt, n: 1, size: $("image-size").value, response_format: "b64_json" };
    if (state.mode === "advanced") { body.steps = numOrUndef("image-steps"); body.seed = numOrUndef("image-seed"); }
    image.ctl = new AbortController();
    $("image-send").disabled = true; $("image-stop").hidden = false; $("image-out").hidden = true;
    setBanner("image-state", "busy", "generating (the server reports no progress for this route; Cancel drops the request)");
    try {
      var j = await api("POST", "/v1/images/generations", body, image.ctl.signal);
      var item = j && j.data && j.data[0];
      var src = item && (item.b64_json ? "data:image/png;base64," + item.b64_json : (item.url && item.url.indexOf("data:image/") === 0 ? item.url : ""));
      if (!src) throw new ApiError(502, null, "the server returned no image data");
      $("image-out").src = src; $("image-out").hidden = false;
      setBanner("image-state", "ok", "image generated.");
    } catch (e) {
      var d = describe(e);
      setBanner("image-state", d.kind, d.message);
    } finally { image.ctl = null; $("image-send").disabled = false; $("image-stop").hidden = true; }
  }

  /* ---------- Performance ---------- */
  async function refreshPerf() {
    try {
      var rt = await api("GET", "/hcs/v1/runtime");
      state.runtime = rt;
      var mem = await api("GET", "/hcs/v1/memory");
      var dev = await api("GET", "/hcs/v1/devices");
      var a = $("perf-runtime"), b = $("perf-memory"), c = $("perf-jobs");
      clear(a); clear(b); clear(c);
      fillDl(a, rt); fillDl(b, mem);
      Object.keys(state.jobs).slice(-12).forEach(function (id) { c.appendChild(el("dt", "", id + " (" + state.jobs[id].name + ")")); c.appendChild(el("dd", "", state.jobs[id].state)); });
      if (!Object.keys(state.jobs).length) { c.appendChild(el("dt", "", "none")); c.appendChild(el("dd", "", "")); }
      text($("perf-devices"), JSON.stringify(dev, null, 2));
      setBanner("perf-state", "idle");
      setConn(true, "connected");
    } catch (e) { var d = describe(e); setBanner("perf-state", d.kind, d.message); }
  }
  function startPerf() { stopPerf(); refreshPerf(); var s = Math.max(1, Math.min(60, Number($("set-interval").value) || DEFAULTS.interval)); state.perfTimer = setInterval(function () { if ($("perf-auto").checked && !document.hidden) refreshPerf(); }, s * 1000); }
  function stopPerf() { if (state.perfTimer) { clearInterval(state.perfTimer); state.perfTimer = 0; } }

  /* ---------- advanced reset ---------- */
  var RESETS = {
    chat: { "chat-system": "", "chat-temp": "0.7", "chat-max": "256", "chat-seed": "" },
    vision: { "vision-max": "256", "vision-temp": "0" },
    image: { "image-steps": "20", "image-seed": "" }
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
    Array.prototype.forEach.call(document.querySelectorAll("input[name=mode]"), function (r) { r.addEventListener("change", function () { if (r.checked) setMode(r.value); }); });
    Array.prototype.forEach.call(document.querySelectorAll("[data-reset]"), function (b) {
      b.addEventListener("click", function () { var m = RESETS[b.dataset.reset]; Object.keys(m).forEach(function (k) { $(k).value = m[k]; }); });
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
    $("chat-clear").addEventListener("click", function () { if (chat.ctl) return; chat.history = []; clear($("chat-log")); text($("chat-meta"), ""); setBanner("chat-state", "idle"); });
    $("vision-file").addEventListener("change", onVisionFile);
    $("vision-form").addEventListener("submit", sendVision);
    $("vision-stop").addEventListener("click", stopVision);
    $("image-form").addEventListener("submit", sendImage);
    $("image-stop").addEventListener("click", function () { if (image.ctl) image.ctl.abort(); });
    $("models-refresh").addEventListener("click", function () { refreshModels().catch(function (e) { var d = describe(e); setBanner("models-state", d.kind, d.message); }); });
    $("perf-refresh").addEventListener("click", refreshPerf);
    connect();
  }
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init); else init();
})();
