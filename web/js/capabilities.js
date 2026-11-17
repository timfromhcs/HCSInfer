// Capability detection for the in-browser runtime.
//
// The decision functions are pure and unit-tested. `probeCapabilities` accepts
// the browser globals as arguments so tests can inject fakes and the page can
// pass the real `navigator` / `WebAssembly`. Nothing here assumes WebGPU is
// present: a missing GPU must degrade to the WASM path, never crash.

import { asLabel } from "./format.js";

export const BACKEND = Object.freeze({
  webgpu: "webgpu",
  wasm: "wasm",
  none: "none",
});

/** Pick the inference backend from what is actually supported. */
export function chooseBackend({ webgpuAvailable, wasmAvailable } = {}) {
  if (webgpuAvailable) {
    return BACKEND.webgpu;
  }
  if (wasmAvailable) {
    return BACKEND.wasm;
  }
  return BACKEND.none;
}

/** Human-readable backend name. */
export function describeBackend(backend) {
  switch (backend) {
    case BACKEND.webgpu:
      return "WebGPU (GPU)";
    case BACKEND.wasm:
      return "WebAssembly (CPU)";
    default:
      return "unavailable";
  }
}

/** True when the runtime data type should be a 16-bit/4-bit WebGPU mix. */
export function prefersFp16(adapter) {
  if (!adapter || typeof adapter !== "object") {
    return false;
  }
  const text = `${adapter.name ?? ""} ${adapter.architecture ?? ""} ${adapter.description ?? ""}`.toLowerCase();
  // Conservative: only assume a 16-bit-friendly path for vendors/architectures
  // known to run it well. Everything else uses the portable integer
  // quantisation. This is a heuristic for the default, never a correctness claim.
  const friendly = ["apple", "nvidia", "amd", "radeon", "rdna", "intel", "arc"];
  return friendly.some((needle) => text.includes(needle));
}

/**
 * Normalise a GPUAdapterInfo (or a plain object) into a stable shape. Missing
 * fields are reported as "unknown", never guessed.
 */
export function summariseAdapter(info) {
  if (!info || typeof info !== "object") {
    return { name: "unknown", vendor: "unknown", architecture: "unknown", description: "unknown" };
  }
  return {
    name: asLabel(info.name ?? info.device),
    vendor: asLabel(info.vendor),
    architecture: asLabel(info.architecture),
    description: asLabel(info.description),
  };
}

const TIMED_OUT = Symbol("probe-timeout");

/**
 * Resolve `promise`, or `sentinel` if it does not settle within `ms`. A browser
 * can expose `navigator.gpu` while `requestAdapter()` never settles (for example
 * a headless browser started with --disable-gpu); that must degrade to the WASM
 * path, not hang the probe forever.
 */
function withTimeout(promise, ms, sentinel) {
  if (!Number.isFinite(ms) || ms <= 0) {
    return promise;
  }
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => resolve(sentinel), ms);
    promise.then(
      (value) => {
        clearTimeout(timer);
        resolve(value);
      },
      (error) => {
        clearTimeout(timer);
        reject(error);
      },
    );
  });
}

/**
 * Probe the runtime. `deps` carries the browser globals so this is testable:
 *   { gpu, wasm, navigator, crossOriginIsolated, gpuTimeoutMs }
 * Returns a serialisable report; errors are captured, not thrown.
 */
export async function probeCapabilities(deps = {}) {
  const {
    gpu = null,
    wasm = null,
    navigator: nav = null,
    crossOriginIsolated = false,
    gpuTimeoutMs = 4000,
  } = deps;

  const report = {
    backend: BACKEND.none,
    webgpu: { available: false, adapter: null, features: [], limits: null },
    wasm: {
      available: Boolean(wasm) && typeof wasm.validate === "function",
      simd: false,
      threads: 1,
      crossOriginIsolated: Boolean(crossOriginIsolated),
    },
    cpu: { hardwareConcurrency: 0, deviceMemoryGb: null },
    errors: [],
  };

  if (nav && typeof nav === "object") {
    if (Number.isFinite(nav.hardwareConcurrency)) {
      report.cpu.hardwareConcurrency = nav.hardwareConcurrency;
    }
    if (Number.isFinite(nav.deviceMemory)) {
      report.cpu.deviceMemoryGb = nav.deviceMemory;
    }
  }

  if (report.wasm.available) {
    try {
      // SIMD probe: a tiny module that uses a v128 instruction.
      report.wasm.simd = wasm.validate(
        new Uint8Array([
          0, 97, 115, 109, 1, 0, 0, 0, 1, 5, 1, 96, 0, 1, 123, 3, 2, 1, 0, 10, 10, 1, 8, 0, 65, 0, 253, 15, 253,
          98, 11,
        ]),
      );
    } catch {
      report.wasm.simd = false;
    }
    if (typeof SharedArrayBuffer !== "undefined" && crossOriginIsolated) {
      report.wasm.threads = Math.max(1, Math.min(4, report.cpu.hardwareConcurrency || 1));
    }
  }

  if (gpu && typeof gpu === "object" && typeof gpu.requestAdapter === "function") {
    try {
      const adapter = await withTimeout(gpu.requestAdapter(), gpuTimeoutMs, TIMED_OUT);
      if (adapter === TIMED_OUT) {
        report.errors.push(
          `WebGPU adapter probe did not settle within ${gpuTimeoutMs} ms; using ${report.wasm.available ? "WASM" : "no backend"}`,
        );
      } else if (adapter) {
        report.webgpu.available = true;
        if (typeof adapter.info === "object" && adapter.info) {
          report.webgpu.adapter = summariseAdapter(adapter.info);
        } else if (typeof adapter.requestAdapterInfo === "function") {
          report.webgpu.adapter = summariseAdapter(await adapter.requestAdapterInfo());
        } else {
          report.webgpu.adapter = summariseAdapter(null);
        }
        if (adapter.features && typeof adapter.features[Symbol.iterator] === "function") {
          report.webgpu.features = Array.from(adapter.features);
        }
        if (adapter.limits) {
          report.webgpu.limits = {
            maxBufferSize: adapter.limits.maxBufferSize ?? null,
            maxComputeWorkgroupSizeX: adapter.limits.maxComputeWorkgroupSizeX ?? null,
            maxComputeInvocationsPerWorkgroup: adapter.limits.maxComputeInvocationsPerWorkgroup ?? null,
          };
        }
      }
    } catch (err) {
      report.errors.push(`WebGPU adapter probe failed: ${err && err.message ? err.message : String(err)}`);
    }
  }

  report.backend = chooseBackend({
    webgpuAvailable: report.webgpu.available,
    wasmAvailable: report.wasm.available,
  });
  return report;
}

/** Short, user-facing line for the Performance panel. */
export function describeCapabilities(report) {
  if (!report) {
    return "not probed";
  }
  const parts = [describeBackend(report.backend)];
  const adapter = report.webgpu && report.webgpu.adapter;
  if (adapter && adapter.name !== "unknown") {
    parts.push(adapter.name);
  }
  if (report.wasm && report.wasm.available && report.backend === BACKEND.wasm) {
    parts.push(report.wasm.simd ? "SIMD" : "no SIMD");
  }
  return parts.join(" - ");
}
