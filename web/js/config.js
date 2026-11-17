// HCS Studio Web - runtime configuration.
//
// Everything that pins a third-party dependency, an allowed origin or a limit
// lives here, so the static checks in tests/static.test.mjs can assert the pins
// and docs can point at one file. This module has no browser dependency and is
// imported by tests.

export const APP = Object.freeze({
  name: "HCS Studio Web",
  product: "HCSmedia",
  version: "0.1.0",
  scope: "v1.1 wave 2 (AGENTS.md section 26.7)",
});

// The only third-party runtime the page loads. Pinned to an exact version; the
// URL is mirrored in the import map in index.html and asserted by the static
// test. The WASM operator runtime (onnxruntime-web) is a transitive dependency
// of this package.
export const GPU_RUNTIME = Object.freeze({
  package: "@huggingface/transformers",
  version: "4.3.2",
  license: "Apache-2.0",
  url: "https://cdn.jsdelivr.net/npm/@huggingface/transformers@4.3.2/dist/transformers.min.js",
  wasmRuntime: "onnxruntime-web (1.31.0-dev, transitive)",
});

// Origins the page is allowed to contact. Must stay in sync with the CSP
// `connect-src` in index.html, vercel.json and tools/serve.mjs.
export const ALLOWED_HOSTS = Object.freeze([
  "https://huggingface.co",
  "https://cdn-lfs.huggingface.co",
  "https://cdn-lfs-us-1.huggingface.co",
  "https://cdn.jsdelivr.net",
]);

export const LIMITS = Object.freeze({
  maxImageBytes: 8 * 1024 * 1024, // client-side check before a data URL is built
  maxPromptChars: 20000,
  maxNewTokens: 2048,
  defaultNewTokens: 256,
  maxHistoryTurns: 24,
  maxSearchResults: 30,
});

// Storage keys. The Hugging Face token is only ever kept in memory and
// sessionStorage (this tab), never in localStorage and never in a URL.
export const STORAGE_KEY = "hcs.studio.web.settings.v1";
export const TOKEN_SESSION_KEY = "hcs.studio.web.hf_token";

// Device selection for the embedding model runtime.
export const DEVICE = Object.freeze({
  webgpu: "webgpu",
  wasm: "wasm",
});
