// Browser-only inference runtime: a thin wrapper over transformers.js.
//
// This module touches browser globals and is intentionally NOT imported by the
// unit tests; the pure decision logic lives in the sibling modules. It applies
// the model's own chat template (via `messages`), streams tokens, supports
// cancellation through InterruptableStoppingCriteria, and adds a Bearer token
// for gated Hub downloads through the documented `env.fetch` hook.

import { DEVICE } from "./config.js";

let libraryPromise = null;

/** Import the pinned runtime exactly once. */
export async function loadLibrary() {
  if (!libraryPromise) {
    // Resolved through the import map in index.html (pinned to GPU_RUNTIME.url).
    libraryPromise = import("@huggingface/transformers");
  }
  return libraryPromise;
}

const realFetch = typeof fetch === "function" ? fetch.bind(globalThis) : null;

/**
 * Configure the library for a local-first, browser-only session.
 * The WASM path is single-threaded because the static host does not send
 * COOP/COEP (needed for SharedArrayBuffer); WebGPU is the primary path.
 */
export function configureEnvironment(lib, { token = null } = {}) {
  const { env } = lib;
  env.allowLocalModels = false;
  env.allowRemoteModels = true;
  env.useBrowserCache = true;
  env.cacheKey = "hcs-studio-web";
  if (env.backends && env.backends.onnx && env.backends.onnx.wasm) {
    env.backends.onnx.wasm.numThreads = 1;
  }
  if (realFetch) {
    const bearer = token && String(token).trim() ? String(token).trim() : null;
    env.fetch = (input, init = {}) => {
      if (!bearer) {
        return realFetch(input, init);
      }
      const url = typeof input === "string" ? input : input && input.url ? input.url : String(input);
      if (/^https:\/\/(huggingface\.co|cdn-lfs[^/]*\.huggingface\.co)\//.test(url)) {
        const headers = new Headers(init.headers || {});
        headers.set("Authorization", `Bearer ${bearer}`);
        return realFetch(input, { ...init, headers });
      }
      return realFetch(input, init);
    };
  }
}

function dtypeFor(device) {
  // WebGPU: 4-bit weights with 16-bit compute where the adapter supports it.
  // WASM: 4-bit integer weights (the portable path).
  return device === "webgpu" ? "q4f16" : "q4";
}

/** Attach an AbortSignal to an InterruptableStoppingCriteria. */
function wireStopper(lib, signal, onAbort) {
  const { InterruptableStoppingCriteria } = lib;
  const stopper = new InterruptableStoppingCriteria();
  if (signal) {
    const stop = () => {
      stopper.interrupt();
      if (onAbort) {
        onAbort();
      }
    };
    if (signal.aborted) {
      stop();
    } else {
      signal.addEventListener("abort", stop, { once: true });
    }
  }
  return stopper;
}

class TextEngine {
  constructor(lib, generator, device) {
    this.lib = lib;
    this.generator = generator;
    this.device = device;
    this.disposed = false;
  }

  /**
   * Generate from a messages array. Returns measured timings and the streamed
   * text; never throws on an aborted signal.
   */
  async generate(messages, opts = {}, hooks = {}) {
    if (this.disposed) {
      throw new Error("engine disposed");
    }
    const { TextStreamer } = this.lib;
    let aborted = false;
    const stopper = wireStopper(this.lib, hooks.signal, () => {
      aborted = true;
    });

    let streamed = "";
    let firstTokenAt = null;
    const started = performance.now();
    const streamer = new TextStreamer(this.generator.tokenizer, {
      skip_prompt: true,
      skip_special_tokens: true,
      callback_function: (text) => {
        if (firstTokenAt === null) {
          firstTokenAt = performance.now();
        }
        streamed += text;
        if (hooks.onToken) {
          hooks.onToken(text);
        }
      },
    });

    const temperature = Number.isFinite(opts.temperature) ? opts.temperature : 0.7;
    try {
      await this.generator(messages, {
        max_new_tokens: opts.maxNewTokens,
        do_sample: temperature > 0,
        temperature: temperature > 0 ? temperature : 1,
        top_p: Number.isFinite(opts.topP) ? opts.topP : 0.95,
        seed: Number.isInteger(opts.seed) ? opts.seed : undefined,
        streamer,
        stopping_criteria: stopper,
      });
    } catch (err) {
      if (aborted || (hooks.signal && hooks.signal.aborted)) {
        return { text: streamed, aborted: true, ...timings(started, firstTokenAt) };
      }
      throw err;
    }
    return { text: streamed, aborted, ...timings(started, firstTokenAt) };
  }

  dispose() {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    try {
      if (this.generator && typeof this.generator.dispose === "function") {
        this.generator.dispose();
      }
    } catch {
      /* dispose is best-effort */
    }
  }
}

function timings(started, firstTokenAt) {
  const totalMs = performance.now() - started;
  return {
    totalMs,
    ttftMs: firstTokenAt === null ? null : firstTokenAt - started,
  };
}

/** Create a text-generation engine (chat). */
export async function createChatEngine({ modelId, device = DEVICE.webgpu, token = null, onProgress } = {}) {
  const lib = await loadLibrary();
  configureEnvironment(lib, { token });
  const generator = await lib.pipeline("text-generation", modelId, {
    device,
    dtype: dtypeFor(device),
    progress_callback: onProgress,
  });
  return new TextEngine(lib, generator, device);
}

/** Create an image-to-text engine (vision). */
export async function createVisionEngine({ modelId, device = DEVICE.webgpu, token = null, onProgress } = {}) {
  const lib = await loadLibrary();
  configureEnvironment(lib, { token });
  const generator = await lib.pipeline("image-to-text", modelId, {
    device,
    dtype: dtypeFor(device),
    progress_callback: onProgress,
  });
  return new TextEngine(lib, generator, device);
}

/** Delete only this app's cached model/WASM entries. Returns the count removed. */
export async function clearModelCache() {
  if (typeof caches === "undefined") {
    return 0;
  }
  let removed = 0;
  const keys = await caches.keys();
  for (const key of keys) {
    if (key === "hcs-studio-web" || key.startsWith("transformers")) {
      await caches.delete(key);
      removed += 1;
    }
  }
  return removed;
}
