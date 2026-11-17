import test from "node:test";
import assert from "node:assert/strict";
import {
  encodeModelId,
  modelSearchUrl,
  modelInfoUrl,
  parseModelInfo,
  parseModelSearch,
  describeLicence,
  describeGating,
} from "../js/hf.js";

test("encodeModelId keeps the org/name slash but escapes segments", () => {
  assert.equal(encodeModelId("org/name"), "org/name");
  assert.equal(encodeModelId("a b/c d"), "a%20b/c%20d");
});

test("URL builders are relative to the Hub API and bounded", () => {
  const search = modelSearchUrl("qwen", { limit: 10 });
  assert.ok(search.startsWith("https://huggingface.co/api/models?"));
  assert.ok(search.includes("search=qwen"));
  assert.ok(search.includes("limit=10"));
  const huge = modelSearchUrl("x", { limit: 100000 });
  assert.ok(huge.includes("limit=30"), "limit must be clamped to maxSearchResults");
  assert.equal(modelInfoUrl("org/name"), "https://huggingface.co/api/models/org/name?blobs=true");
});

test("parseModelInfo sums sizes, detects ONNX and reads the licence", () => {
  const info = parseModelInfo({
    id: "org/name",
    siblings: [
      { rfilename: "onnx/model.onnx", size: 100 },
      { rfilename: "config.json", size: 5 },
      { rfilename: "tokenizer.json" },
    ],
    cardData: { license: "apache-2.0" },
    gated: "manual",
    pipeline_tag: "text-generation",
    downloads: 12,
  });
  assert.equal(info.id, "org/name");
  assert.equal(info.totalBytes, 105);
  assert.equal(info.hasOnnx, true);
  assert.equal(info.licence, "apache-2.0");
  assert.equal(info.gated, true);
  assert.equal(info.gatedMode, "manual");
  assert.equal(info.files.length, 3);
});

test("parseModelInfo tolerates junk", () => {
  assert.equal(parseModelInfo(null), null);
  const info = parseModelInfo({});
  assert.equal(info.totalBytes, 0);
  assert.equal(info.hasOnnx, false);
  assert.equal(info.licence, "unknown");
  assert.equal(info.gated, false);
});

test("parseModelSearch maps entries and drops junk", () => {
  const entries = parseModelSearch([
    { id: "a/b", cardData: { license: "mit" }, gated: false, downloads: 5 },
    null,
    { nope: true },
  ]);
  assert.equal(entries.length, 1);
  assert.equal(entries[0].id, "a/b");
  assert.equal(entries[0].licence, "mit");
  assert.equal(parseModelSearch("not an array").length, 0);
});

test("licence and gating descriptions", () => {
  assert.equal(describeLicence("apache-2.0"), "licence: apache-2.0");
  assert.equal(describeLicence("unknown"), "licence: unknown");
  assert.equal(describeGating(false), "public");
  assert.equal(describeGating(true, "manual"), "gated (approval required)");
  assert.equal(describeGating(true, "auto"), "gated");
});
