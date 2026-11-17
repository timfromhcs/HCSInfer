// Settings: schema, migration and safe persistence.
//
// Pure normalisation is unit-tested. `localStorage` is the only back end; the
// Hugging Face token never goes here (it lives in memory/sessionStorage only).

import { LIMITS, STORAGE_KEY } from "./config.js";
import { clamp } from "./format.js";

export const SETTINGS_VERSION = 1;

export const DEFAULT_SETTINGS = Object.freeze({
  version: SETTINGS_VERSION,
  theme: "system",
  advanced: false,
  chatModel: "onnx-community/Qwen2.5-0.5B-Instruct",
  visionModel: "onnx-community/SmolVLM-256M-Instruct",
  maxNewTokens: LIMITS.defaultNewTokens,
  temperature: 0.7,
  topP: 0.95,
  seed: null,
  keepHistory: true,
});

const THEMES = new Set(["system", "light", "dark"]);

/** Normalise an arbitrary object into valid settings (never throws). */
export function normaliseSettings(raw) {
  const source = raw && typeof raw === "object" ? raw : {};
  const theme = THEMES.has(source.theme) ? source.theme : DEFAULT_SETTINGS.theme;
  return {
    version: SETTINGS_VERSION,
    theme,
    advanced: source.advanced === true,
    chatModel: typeof source.chatModel === "string" && source.chatModel ? source.chatModel : DEFAULT_SETTINGS.chatModel,
    visionModel:
      typeof source.visionModel === "string" && source.visionModel ? source.visionModel : DEFAULT_SETTINGS.visionModel,
    maxNewTokens: Math.round(clamp(source.maxNewTokens ?? DEFAULT_SETTINGS.maxNewTokens, 1, LIMITS.maxNewTokens)),
    temperature: clamp(source.temperature ?? DEFAULT_SETTINGS.temperature, 0, 2),
    topP: clamp(source.topP ?? DEFAULT_SETTINGS.topP, 0, 1),
    seed:
      Number.isFinite(source.seed) && Number.isInteger(source.seed) && source.seed >= 0 ? source.seed : null,
    keepHistory: source.keepHistory !== false,
  };
}

/** Read settings from a Storage-like object; returns defaults on any failure. */
export function readSettings(store) {
  if (!store || typeof store.getItem !== "function") {
    return normaliseSettings(null);
  }
  try {
    const raw = store.getItem(STORAGE_KEY);
    if (!raw) {
      return normaliseSettings(null);
    }
    return normaliseSettings(JSON.parse(raw));
  } catch {
    return normaliseSettings(null);
  }
}

/** Write settings to a Storage-like object. Returns true on success. */
export function writeSettings(store, settings) {
  if (!store || typeof store.setItem !== "function") {
    return false;
  }
  try {
    store.setItem(STORAGE_KEY, JSON.stringify(normaliseSettings(settings)));
    return true;
  } catch {
    return false;
  }
}

/** Reset to defaults and persist. */
export function resetSettings(store) {
  const defaults = normaliseSettings(null);
  writeSettings(store, defaults);
  return defaults;
}
