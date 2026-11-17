// HCS Studio Web - UI wiring.
//
// Vanilla DOM only: no framework, no build step, and (like the embedded local
// UI) no innerHTML/eval anywhere. All model state comes from a real pipeline
// run; there are no fake progress bars or placeholder results.

import { APP, LIMITS, TOKEN_SESSION_KEY, DEVICE } from "./config.js";
import { BACKEND, describeBackend, probeCapabilities, summariseAdapter } from "./capabilities.js";
import { formatBytes, formatDuration, formatRate, asLabel } from "./format.js";
import { listModels, isModelId } from "./catalog.js";
import { buildChatMessages, trimHistory, stripControlTokens, clampPrompt } from "./messages.js";
import { normaliseSettings, readSettings, writeSettings, resetSettings } from "./settings.js";
import { modelInfoUrl, modelSearchUrl, parseModelInfo, parseModelSearch } from "./hf.js";
import { createChatEngine, createVisionEngine, clearModelCache } from "./runtime.js";

const $ = (id) => document.getElementById(id);

const state = {
  settings: normaliseSettings(null),
  caps: null,
  chat: { engine: null, modelId: null, history: [] },
  vision: { engine: null, modelId: null },
  jobs: [],
  busy: false,
  controller: null,
};

// ---------------------------------------------------------------------------
// DOM helpers (no innerHTML)
// ---------------------------------------------------------------------------

function el(tag, props = {}, children = []) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(props)) {
    if (value == null) {
      continue;
    }
    if (key === "class") {
      node.className = value;
    } else if (key === "text") {
      node.textContent = value;
    } else if (key === "dataset") {
      Object.assign(node.dataset, value);
    } else if (key.startsWith("on") && typeof value === "function") {
      node.addEventListener(key.slice(2), value);
    } else {
      node.setAttribute(key, value);
    }
  }
  for (const child of [].concat(children)) {
    if (child == null) {
      continue;
    }
    node.append(child);
  }
  return node;
}

function clear(node) {
  while (node && node.firstChild) {
    node.removeChild(node.firstChild);
  }
}

const BANNER_CLASSES = ["banner-busy", "banner-error", "banner-ok", "banner-unsupported"];

function setBanner(node, kind, text) {
  if (!node) {
    return;
  }
  if (!text) {
    node.hidden = true;
    return;
  }
  node.hidden = false;
  node.classList.remove(...BANNER_CLASSES);
  node.classList.add(`banner-${kind}`);
  node.textContent = text;
}

function describeError(err) {
  const message = err && err.message ? String(err.message) : String(err);
  if (/out of memory|OutOfMemory|allocation/i.test(message)) {
    return `Out of memory in the browser tab. Try a smaller model or the WASM backend. (${message})`;
  }
  if (/401|unauthor|gated|access/i.test(message)) {
    return `The model is gated or the token was rejected. Add a Hugging Face token with access. (${message})`;
  }
  if (/Failed to fetch|NetworkError|net::/i.test(message)) {
    return `Network error while loading model files. Check the connection to huggingface.co. (${message})`;
  }
  return message;
}

function getToken() {
  try {
    return sessionStorage.getItem(TOKEN_SESSION_KEY);
  } catch {
    return null;
  }
}

function setToken(value) {
  try {
    if (value) {
      sessionStorage.setItem(TOKEN_SESSION_KEY, value);
    } else {
      sessionStorage.removeItem(TOKEN_SESSION_KEY);
    }
  } catch {
    /* sessionStorage may be unavailable (private mode); memory only */
  }
}

function recordJob(entry) {
  state.jobs.unshift({ at: new Date().toISOString(), ...entry });
  state.jobs = state.jobs.slice(0, 20);
  renderJobs();
}

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------

function applyTheme() {
  const theme = state.settings.theme;
  if (theme === "system") {
    document.documentElement.removeAttribute("data-theme");
  } else {
    document.documentElement.setAttribute("data-theme", theme);
  }
  for (const radio of document.querySelectorAll('input[name="theme"]')) {
    radio.checked = radio.value === theme;
  }
}

// ---------------------------------------------------------------------------
// Tabs
// ---------------------------------------------------------------------------

function activateTab(panelId) {
  for (const tab of document.querySelectorAll('[role="tab"]')) {
    const selected = tab.dataset.panel === panelId;
    tab.setAttribute("aria-selected", selected ? "true" : "false");
    tab.tabIndex = selected ? 0 : -1;
  }
  for (const panel of document.querySelectorAll('[role="tabpanel"]')) {
    panel.hidden = panel.id !== `panel-${panelId}`;
  }
  const current = $(`tab-${panelId}`);
  if (current) {
    const title = current.querySelector("span");
    $("page-title").textContent = title ? title.textContent : panelId;
  }
  closeSidebar();
}

function wireTabs() {
  const tabs = Array.from(document.querySelectorAll('[role="tab"]'));
  tabs.forEach((tab, index) => {
    tab.addEventListener("click", () => activateTab(tab.dataset.panel));
    tab.addEventListener("keydown", (event) => {
      const keys = ["ArrowDown", "ArrowUp", "Home", "End"];
      if (!keys.includes(event.key)) {
        return;
      }
      event.preventDefault();
      let next = index;
      if (event.key === "ArrowDown") {
        next = (index + 1) % tabs.length;
      } else if (event.key === "ArrowUp") {
        next = (index - 1 + tabs.length) % tabs.length;
      } else if (event.key === "Home") {
        next = 0;
      } else if (event.key === "End") {
        next = tabs.length - 1;
      }
      tabs[next].focus();
      activateTab(tabs[next].dataset.panel);
    });
  });
  $("nav-toggle")?.addEventListener("click", () => {
    const side = $("side");
    const open = side.classList.toggle("open");
    $("nav-toggle").setAttribute("aria-expanded", open ? "true" : "false");
  });
  $("theme-toggle")?.addEventListener("click", () => {
    const order = ["system", "light", "dark"];
    const next = order[(order.indexOf(state.settings.theme) + 1) % order.length];
    state.settings.theme = next;
    applyTheme();
    persist();
  });
}

function closeSidebar() {
  $("side")?.classList.remove("open");
  $("nav-toggle")?.setAttribute("aria-expanded", "false");
}

// ---------------------------------------------------------------------------
// Capabilities / Performance
// ---------------------------------------------------------------------------

async function probe() {
  state.caps = await probeCapabilities({
    gpu: typeof navigator !== "undefined" ? navigator.gpu : null,
    wasm: typeof WebAssembly !== "undefined" ? WebAssembly : null,
    navigator: typeof navigator !== "undefined" ? navigator : null,
    crossOriginIsolated: typeof crossOriginIsolated !== "undefined" ? crossOriginIsolated : false,
  });
  renderCapabilities();
}

function renderCapabilities() {
  const caps = state.caps;
  const backend = caps ? caps.backend : BACKEND.none;
  const devicePill = $("device-pill");
  if (devicePill) {
    devicePill.textContent = describeBackend(backend);
    devicePill.className = `pill ${backend === BACKEND.webgpu ? "pill-ok" : backend === BACKEND.wasm ? "pill-busy" : "pill-err"}`;
  }

  const runtimeList = $("perf-runtime");
  if (runtimeList) {
    clear(runtimeList);
    appendDefs(runtimeList, [
      ["Runtime", `${APP.name} ${APP.version}`],
      ["Inference backend", describeBackend(backend)],
      ["Selected device", backend === BACKEND.wasm ? DEVICE.wasm : backend === BACKEND.webgpu ? DEVICE.webgpu : "none"],
      ["Runs in", "this browser tab (no server inference)"],
    ]);
  }

  const webgpuList = $("perf-webgpu");
  if (webgpuList) {
    clear(webgpuList);
    const adapter = caps ? summariseAdapter(caps.webgpu.adapter) : summariseAdapter(null);
    appendDefs(webgpuList, [
      ["Available", caps && caps.webgpu.available ? "yes" : "no"],
      ["Adapter", adapter.name],
      ["Vendor", adapter.vendor],
      ["Architecture", adapter.architecture],
      ["Features", caps && caps.webgpu.features.length ? String(caps.webgpu.features.length) : "0"],
    ]);
  }

  const wasmList = $("perf-wasm");
  if (wasmList) {
    clear(wasmList);
    appendDefs(wasmList, [
      ["Available", caps && caps.wasm.available ? "yes" : "no"],
      ["SIMD", caps && caps.wasm.simd ? "yes" : "no"],
      ["Threads", caps ? String(caps.wasm.threads) : "unknown"],
      ["Cross-origin isolated", caps && caps.wasm.crossOriginIsolated ? "yes" : "no"],
      ["CPU threads", caps ? asLabel(caps.cpu.hardwareConcurrency, "unknown") : "unknown"],
    ]);
  }
  renderTimingNote();
}

function appendDefs(list, pairs) {
  for (const [term, value] of pairs) {
    list.append(el("dt", { text: term }));
    list.append(el("dd", { text: value == null ? "unknown" : String(value) }));
  }
}

function renderTimingNote() {
  const out = $("perf-timings");
  if (!out) {
    return;
  }
  out.textContent = state.caps
    ? "Timings are measured per run below and in the Chat/Vision banners. Throughput in token/s is approximate (the WebGPU/WASM streamer does not report exact token counts)."
    : "Probing capabilities...";
}

function renderJobs() {
  const list = $("perf-jobs");
  if (!list) {
    return;
  }
  clear(list);
  if (state.jobs.length === 0) {
    list.append(el("dd", { text: "No runs yet this session." }));
    return;
  }
  for (const job of state.jobs.slice(0, 10)) {
    list.append(el("dt", { text: job.kind }));
    const detail = job.detail ? ` - ${job.detail}` : "";
    list.append(el("dd", { text: `${formatDuration(job.ms)}${detail}` }));
  }
}

// ---------------------------------------------------------------------------
// Models
// ---------------------------------------------------------------------------

function renderCatalog() {
  const host = $("models-catalog");
  if (!host) {
    return;
  }
  clear(host);
  for (const model of listModels()) {
    const row = el("li", { class: "model-row" }, [
      el("div", { class: "model-head" }, [
        el("strong", { text: model.id }),
        el("span", { class: `tag tag-${model.task}`, text: model.task }),
      ]),
      el("p", { class: "note", text: `${model.params} params - ${model.licenceName} - approx ${formatBytes(model.approxBytes)}` }),
      el("p", { class: "note", text: model.note }),
      el("div", { class: "row" }, [
        el("button", { type: "button", class: "ghost", text: "Use in Chat", onclick: () => useModel(model, "chat") }),
        el("button", { type: "button", class: "ghost", text: "Use in Vision", onclick: () => useModel(model, "vision") }),
        el("button", { type: "button", class: "ghost", text: "Details", onclick: () => showModelInfo(model.id) }),
      ]),
    ]);
    host.append(row);
  }
}

function useModel(model, target) {
  if (target === "chat" && model.task === "chat") {
    state.settings.chatModel = model.id;
    persist();
    syncModelSelects();
    activateTab("chat");
  } else if (target === "vision" && model.task === "vision") {
    state.settings.visionModel = model.id;
    persist();
    syncModelSelects();
    activateTab("vision");
  } else {
    setBanner($("models-state"), "unsupported", `${model.id} is a ${model.task} model; pick the matching tab.`);
  }
}

function syncModelSelects() {
  const fill = (select, task, current) => {
    if (!select) {
      return;
    }
    clear(select);
    const items = listModels(task);
    if (!items.some((m) => m.id === current)) {
      select.append(el("option", { value: current, text: current }));
    }
    for (const model of items) {
      select.append(el("option", { value: model.id, text: `${model.id} (${model.params})` }));
    }
    select.value = current;
  };
  fill($("chat-model"), "chat", state.settings.chatModel);
  fill($("vision-model"), "vision", state.settings.visionModel);
}

async function searchHub(query) {
  const banner = $("models-state");
  const list = $("models-results");
  if (!list) {
    return;
  }
  clear(list);
  setBanner(banner, "busy", `Searching the Hub for "${query}"...`);
  try {
    const response = await fetch(modelSearchUrl(query, { limit: LIMITS.maxSearchResults }), {
      headers: getToken() ? { Authorization: `Bearer ${getToken()}` } : {},
    });
    if (!response.ok) {
      throw new Error(`Hub search returned ${response.status}`);
    }
    const entries = parseModelSearch(await response.json());
    setBanner(banner, "ok", `${entries.length} result(s) for "${query}".`);
    for (const entry of entries) {
      list.append(
        el("li", { class: "model-row" }, [
          el("div", { class: "model-head" }, [
            el("strong", { text: entry.id }),
            el("span", { class: "tag", text: entry.gated ? "gated" : "public" }),
          ]),
          el("p", { class: "note", text: `${entry.licence} - ${entry.pipelineTag || "unknown task"}` }),
          el("div", { class: "row" }, [el("button", { type: "button", class: "ghost", text: "Details", onclick: () => showModelInfo(entry.id) })]),
        ]),
      );
    }
  } catch (err) {
    setBanner(banner, "error", describeError(err));
  }
}

async function showModelInfo(id) {
  const banner = $("models-state");
  const box = $("models-info");
  if (!box) {
    return;
  }
  clear(box);
  box.append(el("p", { class: "note", text: `Loading metadata for ${id}...` }));
  try {
    const response = await fetch(modelInfoUrl(id), {
      headers: getToken() ? { Authorization: `Bearer ${getToken()}` } : {},
    });
    if (!response.ok) {
      throw new Error(`Hub model info returned ${response.status}`);
    }
    const info = parseModelInfo(await response.json());
    clear(box);
    appendDefs(appendDl(box, "Model metadata"), [
      ["Id", info.id],
      ["Licence", info.licenceName || info.licence],
      ["Access", info.gated ? info.gatedMode || "gated" : "public"],
      ["Task", info.pipelineTag || "unknown"],
      ["Files", String(info.files.length)],
      ["Total size", formatBytes(info.totalBytes)],
      ["ONNX present", info.hasOnnx ? "yes" : "no"],
    ]);
    setBanner(banner, "ok", "Metadata loaded (sizes are from the Hub).");
  } catch (err) {
    clear(box);
    setBanner(banner, "error", describeError(err));
  }
}

function appendDl(parent, title) {
  const card = el("div", { class: "card" }, [el("h3", { class: "sect first", text: title })]);
  const list = el("dl");
  card.append(list);
  parent.append(card);
  return list;
}

// ---------------------------------------------------------------------------
// Chat
// ---------------------------------------------------------------------------

async function ensureChatEngine(modelId) {
  if (state.chat.engine && state.chat.modelId === modelId) {
    return state.chat.engine;
  }
  if (state.chat.engine) {
    state.chat.engine.dispose();
    state.chat.engine = null;
  }
  const device = state.caps && state.caps.backend === BACKEND.wasm ? DEVICE.wasm : DEVICE.webgpu;
  const banner = $("chat-state");
  setBanner(banner, "busy", `Loading ${modelId} (${describeBackend(device === DEVICE.wasm ? BACKEND.wasm : BACKEND.webgpu)})...`);
  const started = performance.now();
  const engine = await createChatEngine({
    modelId,
    device,
    token: getToken(),
    onProgress: (p) => {
      if (p && p.status === "progress" && p.file) {
        const pct = typeof p.progress === "number" ? `${Math.round(p.progress)}%` : "";
        setBanner(banner, "busy", `Downloading ${p.file} ${pct}`);
      } else if (p && p.status) {
        setBanner(banner, "busy", `${p.status} ${p.file || ""}`.trim());
      }
    },
  });
  const ms = performance.now() - started;
  recordJob({ kind: "chat model load", detail: modelId, ms });
  state.chat.engine = engine;
  state.chat.modelId = modelId;
  setBanner(banner, "ok", `Loaded ${modelId} in ${formatDuration(ms)}.`);
  return engine;
}

async function sendChat(event) {
  event.preventDefault();
  if (state.busy) {
    return;
  }
  const banner = $("chat-state");
  const input = $("chat-input");
  const text = clampPrompt(input.value).trim();
  if (!text) {
    return;
  }
  const modelId = $("chat-model").value || state.settings.chatModel;
  state.settings.chatModel = modelId;
  persist();
  state.busy = true;
  toggleChatBusy(true);
  appendMessage("user", text);
  input.value = "";

  const assistant = appendMessage("assistant", "");
  const controller = new AbortController();
  state.controller = controller;

  try {
    const engine = await ensureChatEngine(modelId);
    const messages = trimHistory(
      buildChatMessages({ system: $("chat-system").value, history: state.chat.history, user: text }),
      LIMITS.maxHistoryTurns,
    );
    const result = await engine.generate(
      messages,
      {
        maxNewTokens: Number($("chat-max").value) || LIMITS.defaultNewTokens,
        temperature: Number($("chat-temp").value),
        topP: 0.95,
        seed: $("chat-seed").value ? Number($("chat-seed").value) : undefined,
      },
      {
        signal: controller.signal,
        onToken: (token) => {
          assistant.textContent += stripControlTokens(token);
        },
      },
    );
    const answer = stripControlTokens(result.text).trim();
    assistant.textContent = answer || "(no output)";
    if (state.settings.keepHistory && answer) {
      state.chat.history = trimHistory(
        [...state.chat.history, { role: "user", content: text }, { role: "assistant", content: answer }],
        LIMITS.maxHistoryTurns,
      );
    }
    const approxTokens = Math.max(1, Math.round(answer.length / 4));
    const rate = formatRate(approxTokens / (result.totalMs / 1000));
    const ttft = result.ttftMs == null ? "n/a" : formatDuration(result.ttftMs);
    setBanner(
      banner,
      result.aborted ? "unsupported" : "ok",
      result.aborted ? "Stopped by the user." : `Done. Time to first token ${ttft}, total ${formatDuration(result.totalMs)}, ~${rate} (approx).`,
    );
    recordJob({ kind: "chat generate", detail: modelId, ms: result.totalMs });
    $("chat-meta").textContent = `${formatDuration(result.totalMs)} - ~${rate}`;
  } catch (err) {
    assistant.classList.add("msg-error");
    assistant.textContent = describeError(err);
    setBanner(banner, "error", describeError(err));
  } finally {
    state.busy = false;
    state.controller = null;
    toggleChatBusy(false);
  }
}

function toggleChatBusy(busy) {
  $("chat-send").disabled = busy;
  $("chat-stop").hidden = !busy;
}

function appendMessage(role, text) {
  const log = $("chat-log");
  $("chat-empty")?.remove();
  const node = el("div", { class: `msg msg-${role}` }, [el("b", { text: role }), el("span", { text })]);
  node.querySelector("span").textContent = text;
  log.append(node);
  log.scrollTop = log.scrollHeight;
  return node.querySelector("span");
}

function stopChat() {
  if (state.controller) {
    state.controller.abort();
  }
}

function clearChat() {
  const log = $("chat-log");
  clear(log);
  log.append(el("p", { class: "empty", id: "chat-empty", text: "Conversation cleared." }));
  state.chat.history = [];
  $("chat-meta").textContent = "";
}

// ---------------------------------------------------------------------------
// Vision
// ---------------------------------------------------------------------------

async function ensureVisionEngine(modelId) {
  if (state.vision.engine && state.vision.modelId === modelId) {
    return state.vision.engine;
  }
  if (state.vision.engine) {
    state.vision.engine.dispose();
    state.vision.engine = null;
  }
  const device = state.caps && state.caps.backend === BACKEND.wasm ? DEVICE.wasm : DEVICE.webgpu;
  const banner = $("vision-state");
  setBanner(banner, "busy", `Loading vision model ${modelId}...`);
  const started = performance.now();
  const engine = await createVisionEngine({
    modelId,
    device,
    token: getToken(),
    onProgress: (p) => {
      if (p && p.status === "progress" && p.file) {
        setBanner(banner, "busy", `Downloading ${p.file} ${typeof p.progress === "number" ? Math.round(p.progress) + "%" : ""}`);
      }
    },
  });
  const ms = performance.now() - started;
  recordJob({ kind: "vision model load", detail: modelId, ms });
  state.vision.engine = engine;
  state.vision.modelId = modelId;
  setBanner(banner, "ok", `Loaded ${modelId} in ${formatDuration(ms)}.`);
  return engine;
}

function readImageAsDataUrl(file) {
  return new Promise((resolve, reject) => {
    if (file.size > LIMITS.maxImageBytes) {
      reject(new Error(`Image is larger than ${formatBytes(LIMITS.maxImageBytes)}.`));
      return;
    }
    const reader = new FileReader();
    reader.onload = () => resolve(String(reader.result));
    reader.onerror = () => reject(new Error("Could not read the image file."));
    reader.readAsDataURL(file);
  });
}

async function analyzeImage(event) {
  event.preventDefault();
  if (state.busy) {
    return;
  }
  const banner = $("vision-state");
  const output = $("vision-out");
  const file = $("vision-file").files[0];
  if (!file) {
    setBanner(banner, "error", "Choose an image first.");
    return;
  }
  const modelId = $("vision-model").value || state.settings.visionModel;
  state.settings.visionModel = modelId;
  persist();
  state.busy = true;
  toggleVisionBusy(true);
  output.textContent = "";

  const controller = new AbortController();
  state.controller = controller;
  try {
    const dataUrl = await readImageAsDataUrl(file);
    const engine = await ensureVisionEngine(modelId);
    const question = clampPrompt($("vision-prompt").value).trim() || "Describe this image.";
    let streamed = "";
    const result = await engine.generate(
      [{ type: "image", url: dataUrl }, { type: "text", text: question }],
      { maxNewTokens: 256, temperature: 0 },
      {
        signal: controller.signal,
        onToken: (token) => {
          streamed += token;
          output.textContent = stripControlTokens(streamed);
        },
      },
    );
    output.textContent = stripControlTokens(result.text).trim() || "(no output)";
    setBanner(banner, result.aborted ? "unsupported" : "ok", result.aborted ? "Stopped." : `Done in ${formatDuration(result.totalMs)}.`);
    recordJob({ kind: "vision analyze", detail: modelId, ms: result.totalMs });
  } catch (err) {
    setBanner(banner, "error", describeError(err));
    output.textContent = describeError(err);
  } finally {
    state.busy = false;
    state.controller = null;
    toggleVisionBusy(false);
  }
}

function toggleVisionBusy(busy) {
  $("vision-send").disabled = busy;
  $("vision-stop").hidden = !busy;
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

function persist() {
  state.settings = normaliseSettings(state.settings);
  writeSettings(window.localStorage, state.settings);
}

function wireSettings() {
  for (const radio of document.querySelectorAll('input[name="theme"]')) {
    radio.addEventListener("change", () => {
      state.settings.theme = radio.value;
      applyTheme();
      persist();
    });
  }
  $("set-advanced")?.addEventListener("change", (event) => {
    state.settings.advanced = event.target.checked;
    persist();
    applyAdvanced();
  });
  $("set-keep-history")?.addEventListener("change", (event) => {
    state.settings.keepHistory = event.target.checked;
    persist();
  });
  $("set-forget")?.addEventListener("click", () => {
    setToken(null);
    $("authbar").hidden = true;
    setBanner($("settings-state"), "ok", "Token forgotten for this tab.");
  });
  $("set-clear-cache")?.addEventListener("click", async () => {
    try {
      const removed = await clearModelCache();
      setBanner($("settings-state"), "ok", `Cleared ${removed} browser cache group(s).`);
    } catch (err) {
      setBanner($("settings-state"), "error", describeError(err));
    }
  });
  $("set-reset")?.addEventListener("click", () => {
    state.settings = resetSettings(window.localStorage);
    applyTheme();
    applyAdvanced();
    syncModelSelects();
    setBanner($("settings-state"), "ok", "Settings reset to defaults.");
  });
}

function applyAdvanced() {
  for (const node of document.querySelectorAll("[data-advanced]")) {
    node.hidden = !state.settings.advanced;
  }
  for (const radio of document.querySelectorAll('input[name="mode"]')) {
    radio.checked = (radio.value === "advanced") === state.settings.advanced;
  }
  const toggle = $("set-advanced");
  if (toggle) {
    toggle.checked = state.settings.advanced;
  }
}

function wireAdvancedToggle() {
  for (const radio of document.querySelectorAll('input[name="mode"]')) {
    radio.addEventListener("change", () => {
      state.settings.advanced = radio.value === "advanced";
      persist();
      applyAdvanced();
    });
  }
}

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------

function wireForms() {
  $("chat-form")?.addEventListener("submit", sendChat);
  $("chat-stop")?.addEventListener("click", stopChat);
  $("chat-clear")?.addEventListener("click", clearChat);
  $("vision-form")?.addEventListener("submit", analyzeImage);
  $("vision-stop")?.addEventListener("click", stopChat);
  $("vision-file")?.addEventListener("change", (event) => {
    const file = event.target.files[0];
    const preview = $("vision-preview");
    if (file && preview) {
      preview.src = URL.createObjectURL(file);
      preview.hidden = false;
    }
  });
  $("models-search-btn")?.addEventListener("click", () => searchHub($("models-search-input").value));
  $("models-info-btn")?.addEventListener("click", () => showModelInfo($("models-id").value));
  $("auth-form")?.addEventListener("submit", (event) => {
    event.preventDefault();
    setToken($("auth-token").value.trim());
    $("authbar").hidden = true;
    setBanner($("settings-state"), "ok", "Token stored for this tab only.");
  });
  $("perf-refresh")?.addEventListener("click", probe);
  $("chat-model")?.addEventListener("change", () => {
    state.settings.chatModel = $("chat-model").value;
    persist();
  });
  $("vision-model")?.addEventListener("change", () => {
    state.settings.visionModel = $("vision-model").value;
    persist();
  });
}

function boot() {
  state.settings = readSettings(window.localStorage);
  if (!isModelId(state.settings.chatModel)) {
    state.settings.chatModel = normaliseSettings(null).chatModel;
  }
  wireTabs();
  wireForms();
  wireSettings();
  wireAdvancedToggle();
  renderCatalog();
  syncModelSelects();
  applyTheme();
  applyAdvanced();
  $("auth-note").textContent = getToken()
    ? "A Hugging Face token is kept in this tab for gated models."
    : "Public models need no token. A gated model needs your own Hugging Face token; it is kept only in this tab (session storage) and sent only to huggingface.co.";
  const footer = $("ver");
  if (footer) {
    footer.textContent = `${APP.name} ${APP.version}`;
  }
  activateTab("chat");
  probe();
}

if (typeof document !== "undefined") {
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", boot);
  } else {
    boot();
  }
}
