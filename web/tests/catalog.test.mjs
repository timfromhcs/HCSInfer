import test from "node:test";
import assert from "node:assert/strict";
import { CATALOG, listModels, findModel, isModelId, estimatePeakBytes } from "../js/catalog.js";

test("the catalog is non-empty and every entry is well formed", () => {
  const all = listModels();
  assert.ok(all.length >= 3);
  for (const entry of all) {
    assert.ok(isModelId(entry.id), `${entry.id} is not a valid model id`);
    assert.ok(entry.task === "chat" || entry.task === "vision");
    assert.ok(entry.approxBytes > 0);
    assert.ok(typeof entry.licenceName === "string" && entry.licenceName.length > 0);
  }
});

test("listModels filters by task", () => {
  assert.ok(listModels("chat").every((m) => m.task === "chat"));
  assert.ok(listModels("vision").every((m) => m.task === "vision"));
  assert.equal(listModels().length, listModels("chat").length + listModels("vision").length);
});

test("findModel returns the entry or null", () => {
  const first = listModels()[0];
  assert.equal(findModel(first.id), first);
  assert.equal(findModel("does/not-exist"), null);
});

test("isModelId accepts org/name and rejects traversal and junk", () => {
  assert.equal(isModelId("onnx-community/Qwen2.5-0.5B-Instruct"), true);
  assert.equal(isModelId("org/name"), true);
  assert.equal(isModelId("nameonly"), false);
  assert.equal(isModelId("org/"), false);
  assert.equal(isModelId("/name"), false);
  assert.equal(isModelId("a/../b"), false);
  assert.equal(isModelId("a/b/c"), false);
  assert.equal(isModelId(""), false);
  assert.equal(isModelId(123), false);
  assert.equal(isModelId("  org/name  "), true);
});

test("estimatePeakBytes is above the weight size and null for bad input", () => {
  const entry = listModels()[0];
  assert.ok(estimatePeakBytes(entry) > entry.approxBytes);
  assert.equal(estimatePeakBytes(null), null);
  assert.equal(estimatePeakBytes({ approxBytes: Number.NaN }), null);
});

test("catalog is exported as a frozen array", () => {
  assert.ok(Array.isArray(CATALOG));
  assert.equal(Object.isFrozen(CATALOG), true);
});
