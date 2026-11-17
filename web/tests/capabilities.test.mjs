import test from "node:test";
import assert from "node:assert/strict";
import {
  BACKEND,
  chooseBackend,
  describeBackend,
  prefersFp16,
  summariseAdapter,
  probeCapabilities,
  describeCapabilities,
} from "../js/capabilities.js";

test("chooseBackend prefers WebGPU, then WASM, then none", () => {
  assert.equal(chooseBackend({ webgpuAvailable: true, wasmAvailable: true }), BACKEND.webgpu);
  assert.equal(chooseBackend({ webgpuAvailable: true, wasmAvailable: false }), BACKEND.webgpu);
  assert.equal(chooseBackend({ webgpuAvailable: false, wasmAvailable: true }), BACKEND.wasm);
  assert.equal(chooseBackend({}), BACKEND.none);
  assert.equal(chooseBackend(), BACKEND.none);
});

test("describeBackend is explicit", () => {
  assert.equal(describeBackend(BACKEND.webgpu), "WebGPU (GPU)");
  assert.equal(describeBackend(BACKEND.wasm), "WebAssembly (CPU)");
  assert.equal(describeBackend(BACKEND.none), "unavailable");
});

test("summariseAdapter never guesses missing fields", () => {
  const empty = summariseAdapter(null);
  assert.deepEqual(empty, { name: "unknown", vendor: "unknown", architecture: "unknown", description: "unknown" });
  const partial = summariseAdapter({ name: "Radeon 680M", vendor: "amd" });
  assert.equal(partial.name, "Radeon 680M");
  assert.equal(partial.vendor, "amd");
  assert.equal(partial.architecture, "unknown");
});

test("prefersFp16 only for known 16-bit-friendly vendors", () => {
  assert.equal(prefersFp16({ name: "Apple M2" }), true);
  assert.equal(prefersFp16({ description: "NVIDIA GeForce" }), true);
  assert.equal(prefersFp16({ architecture: "rdna-2" }), true);
  assert.equal(prefersFp16({ name: "mystery gpu" }), false);
  assert.equal(prefersFp16(null), false);
});

test("probeCapabilities reports none when nothing is available", async () => {
  const report = await probeCapabilities({});
  assert.equal(report.backend, BACKEND.none);
  assert.equal(report.webgpu.available, false);
  assert.equal(report.wasm.available, false);
});

test("probeCapabilities selects WASM when only WebAssembly exists", async () => {
  const report = await probeCapabilities({
    wasm: { validate: () => true },
    navigator: { hardwareConcurrency: 8 },
  });
  assert.equal(report.backend, BACKEND.wasm);
  assert.equal(report.wasm.available, true);
  assert.equal(report.wasm.simd, true);
  assert.equal(report.cpu.hardwareConcurrency, 8);
  assert.equal(describeCapabilities(report), "WebAssembly (CPU) - SIMD");
});

test("probeCapabilities reads a WebGPU adapter", async () => {
  const fakeGpu = {
    requestAdapter: async () => ({
      info: { name: "AMD Radeon 680M", vendor: "amd", architecture: "rdna-2" },
      features: new Set(["shader-f16"]),
      limits: { maxBufferSize: 2147483648, maxComputeWorkgroupSizeX: 256 },
    }),
  };
  const report = await probeCapabilities({ gpu: fakeGpu, wasm: { validate: () => true } });
  assert.equal(report.backend, BACKEND.webgpu);
  assert.equal(report.webgpu.available, true);
  assert.equal(report.webgpu.adapter.name, "AMD Radeon 680M");
  assert.deepEqual(report.webgpu.features, ["shader-f16"]);
  assert.equal(report.webgpu.limits.maxBufferSize, 2147483648);
});

test("probeCapabilities does not hang on a never-settling WebGPU adapter", async () => {
  // A headless browser can expose navigator.gpu while requestAdapter() never
  // settles; the probe must time out and fall back to WASM.
  const fakeGpu = { requestAdapter: () => new Promise(() => {}) };
  const report = await probeCapabilities({ gpu: fakeGpu, wasm: { validate: () => true }, gpuTimeoutMs: 20 });
  assert.equal(report.webgpu.available, false);
  assert.equal(report.backend, BACKEND.wasm);
  assert.equal(report.errors.length, 1);
});

test("probeCapabilities captures a failing GPU probe instead of throwing", async () => {
  const fakeGpu = {
    requestAdapter: async () => {
      throw new Error("no adapter");
    },
  };
  const report = await probeCapabilities({ gpu: fakeGpu, wasm: { validate: () => true } });
  assert.equal(report.webgpu.available, false);
  assert.equal(report.backend, BACKEND.wasm);
  assert.equal(report.errors.length, 1);
});
