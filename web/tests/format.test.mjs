import test from "node:test";
import assert from "node:assert/strict";
import { formatBytes, formatDuration, tokensPerSecond, formatRate, clamp, formatPercent, asLabel } from "../js/format.js";

test("formatBytes uses binary units and never lies about unknown input", () => {
  assert.equal(formatBytes(0), "0 B");
  assert.equal(formatBytes(512), "512 B");
  assert.equal(formatBytes(1024), "1.00 KiB");
  assert.equal(formatBytes(1536), "1.50 KiB");
  assert.equal(formatBytes(3 * 1024), "3.00 KiB");
  assert.equal(formatBytes(5 * 1024 * 1024), "5.00 MiB");
  assert.equal(formatBytes(400 * 1024 * 1024), "400 MiB");
  assert.equal(formatBytes(null), "unknown");
  assert.equal(formatBytes(undefined), "unknown");
  assert.equal(formatBytes(-1), "unknown");
  assert.equal(formatBytes(Number.NaN), "unknown");
  assert.equal(formatBytes("nope"), "unknown");
  assert.equal(formatBytes(2048, { decimals: 3 }), "2.000 KiB");
});

test("formatDuration is human readable across scales", () => {
  assert.equal(formatDuration(0), "0 ms");
  assert.equal(formatDuration(500), "500 ms");
  assert.equal(formatDuration(1500), "1.50 s");
  assert.equal(formatDuration(30000), "30.0 s");
  assert.equal(formatDuration(65000), "1 m 5 s");
  assert.equal(formatDuration(3_900_000), "1 h 5 m");
  assert.equal(formatDuration(-5), "unknown");
  assert.equal(formatDuration(Number.NaN), "unknown");
});

test("tokensPerSecond and formatRate", () => {
  assert.equal(tokensPerSecond(10, 1000), 10);
  assert.equal(tokensPerSecond(0, 1000), null);
  assert.equal(tokensPerSecond(10, 0), null);
  assert.equal(tokensPerSecond(Number.NaN, 1000), null);
  assert.equal(formatRate(9.5), "9.50 tok/s");
  assert.equal(formatRate(42.34), "42.3 tok/s");
  assert.equal(formatRate(null), "n/a");
  assert.equal(formatRate(0), "n/a");
});

test("clamp stays in range and fails safe", () => {
  assert.equal(clamp(3, 1, 5), 3);
  assert.equal(clamp(10, 1, 5), 5);
  assert.equal(clamp(-10, 1, 5), 1);
  assert.equal(clamp(Number.NaN, 1, 5), 1);
  assert.equal(clamp("3.5", 0, 2), 2);
});

test("formatPercent and asLabel handle bad input", () => {
  assert.equal(formatPercent(0.5), "50%");
  assert.equal(formatPercent(0.1234, 1), "12.3%");
  assert.equal(formatPercent(Number.NaN), "unknown");
  assert.equal(asLabel("  hi "), "hi");
  assert.equal(asLabel(""), "unknown");
  assert.equal(asLabel(42), "unknown");
  assert.equal(asLabel(null, "fallback"), "fallback");
});
