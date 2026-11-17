// Curated model catalog for the browser demo.
//
// IMPORTANT (AGENTS.md 0.4, 26.7): these entries are *catalogued*, not verified
// in this browser. A model is only "Verified in browser" once it has actually
// been loaded and run here, and the Performance panel records per-run timings
// for exactly that reason. Sizes are approximate and re-measured from the Hub
// when the Models panel fetches model info.

/** @typedef {{id: string, task: "chat"|"vision", params: string, licence: string, licenceName: string, approxBytes: number, device: "webgpu"|"wasm"|"both", note: string}} CatalogEntry */

/** @type {CatalogEntry[]} */
export const CATALOG = Object.freeze([
  {
    id: "onnx-community/Qwen2.5-0.5B-Instruct",
    task: "chat",
    params: "0.5B",
    licence: "apache-2.0",
    licenceName: "Apache License 2.0",
    approxBytes: 350 * 1024 * 1024,
    device: "both",
    note: "Default text model. Small enough for a first WebGPU run on an integrated GPU.",
  },
  {
    id: "onnx-community/SmolLM2-360M-Instruct",
    task: "chat",
    params: "360M",
    licence: "apache-2.0",
    licenceName: "Apache License 2.0",
    approxBytes: 260 * 1024 * 1024,
    device: "both",
    note: "Smallest chat option; useful to prove the pipeline before a larger download.",
  },
  {
    id: "onnx-community/SmolVLM-256M-Instruct",
    task: "vision",
    params: "256M",
    licence: "apache-2.0",
    licenceName: "Apache License 2.0",
    approxBytes: 300 * 1024 * 1024,
    device: "both",
    note: "Default vision model. One image, one question, streamed answer.",
  },
  {
    id: "onnx-community/Florence-2-base-ft",
    task: "vision",
    params: "0.23B",
    licence: "mit",
    licenceName: "MIT License",
    approxBytes: 550 * 1024 * 1024,
    device: "both",
    note: "Captioning / OCR style prompts. Second vision option.",
  },
]);

/** All catalog entries, optionally filtered by task. */
export function listModels(task) {
  if (!task) {
    return CATALOG.slice();
  }
  return CATALOG.filter((m) => m.task === task);
}

/** Find a catalog entry by id (case-sensitive, exact). */
export function findModel(id) {
  return CATALOG.find((m) => m.id === id) ?? null;
}

/** True when `id` looks like a valid "org/name" Hugging Face model id. */
export function isModelId(id) {
  if (typeof id !== "string") {
    return false;
  }
  const s = id.trim();
  if (s.length < 3 || s.length > 200 || s.includes("..") || s.startsWith("/") || s.endsWith("/")) {
    return false;
  }
  return /^[A-Za-z0-9][A-Za-z0-9._-]*\/[A-Za-z0-9][A-Za-z0-9._-]*$/.test(s);
}

/**
 * Rough peak-memory estimate for a model, to warn before an out-of-memory
 * failure. Deliberately conservative and labelled an estimate everywhere it is
 * shown. `bytesPerParam` defaults to a 4-bit weight plus a float16 KV overhead.
 */
export function estimatePeakBytes(entry, { bytesPerParam = 0.9, kvFactor = 0.35 } = {}) {
  if (!entry || !Number.isFinite(entry.approxBytes)) {
    return null;
  }
  return Math.round(entry.approxBytes * (1 + kvFactor));
}
