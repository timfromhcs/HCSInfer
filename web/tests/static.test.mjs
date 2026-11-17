// Static security/consistency checks over the shipped files.
//
// These assert the properties AGENTS.md 26.7 requires of the web demo and that a
// unit test cannot reach through the DOM: the CSP, the single pinned third-party
// runtime, no analytics, no HTML-injection sinks and a dependency-free package.

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync, readdirSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { GPU_RUNTIME, ALLOWED_HOSTS } from "../js/config.js";

const here = dirname(fileURLToPath(import.meta.url));
const webRoot = join(here, "..");

const read = (rel) => readFileSync(join(webRoot, rel), "utf8");
const jsFiles = readdirSync(join(webRoot, "js")).filter((f) => f.endsWith(".js"));

const FORBIDDEN_JS = [
  ".innerHTML",
  ".outerHTML",
  "insertAdjacentHTML",
  "document.write",
  "eval(",
  "new Function(",
];

const ANALYTICS_SIGNATURES = [
  "googletagmanager",
  "google-analytics.com",
  "gtag(",
  "plausible.io",
  "posthog",
  "mixpanel",
  "sentry",
  "segment.com",
];

test("there is JavaScript to check", () => {
  assert.ok(jsFiles.length >= 6, `expected several js modules, found ${jsFiles.length}`);
});

test("no HTML-injection or dynamic-code sinks in any module", () => {
  for (const file of jsFiles) {
    const source = read(join("js", file));
    for (const needle of FORBIDDEN_JS) {
      assert.ok(!source.includes(needle), `${file} must not contain ${needle}`);
    }
  }
});

test("no analytics or third-party tracker is referenced anywhere", () => {
  const files = ["index.html", "app.css", ...jsFiles.map((f) => join("js", f))];
  for (const file of files) {
    const source = read(file).toLowerCase();
    for (const needle of ANALYTICS_SIGNATURES) {
      assert.ok(!source.includes(needle), `${file} must not reference ${needle}`);
    }
  }
});

test("the import map pins exactly the configured runtime", () => {
  const html = read("index.html");
  const match = html.match(/<script type="importmap">([\s\S]*?)<\/script>/);
  assert.ok(match, "index.html must contain an import map");
  const map = JSON.parse(match[1]);
  assert.equal(map.imports["@huggingface/transformers"], GPU_RUNTIME.url);
  assert.ok(html.includes(`transformers@${GPU_RUNTIME.version}`), "the pinned version must be visible in index.html");
});

test("the meta CSP is strict and matches the allowed hosts", () => {
  const html = read("index.html");
  const match = html.match(/http-equiv="Content-Security-Policy" content="([^"]+)"/);
  assert.ok(match, "index.html must set a meta CSP");
  const csp = match[1];
  assert.ok(csp.includes("default-src 'none'"), "default-src must be 'none'");
  assert.ok(csp.includes("script-src 'self' 'wasm-unsafe-eval' https://cdn.jsdelivr.net"), "script-src must be self + wasm + the pinned CDN only");
  assert.ok(!csp.includes("unsafe-inline"), "inline script/style must stay disallowed");
  assert.ok(csp.includes("object-src 'none'"));
  assert.ok(csp.includes("base-uri 'none'"));
  assert.ok(csp.includes("form-action 'none'"));
  const connect = /connect-src ([^;]+);/.exec(csp);
  assert.ok(connect, "connect-src must be present");
  for (const host of ALLOWED_HOSTS) {
    assert.ok(connect[1].includes(host), `connect-src must allow ${host}`);
  }
});

test("index.html has no inline event handlers", () => {
  // Strip comments first so a prose word like "only =" cannot be mistaken for
  // an attribute; only real markup is checked.
  const markup = read("index.html").replace(/<!--[\s\S]*?-->/g, "");
  assert.ok(!/\son[a-z]+\s*=/i.test(markup), "no inline on* handlers (CSP has no unsafe-inline)");
});

test("vercel.json sets the authoritative security headers", () => {
  const config = JSON.parse(read("vercel.json"));
  const headers = JSON.stringify(config.headers ?? []);
  assert.ok(headers.includes("Content-Security-Policy"), "CSP header required");
  assert.ok(headers.includes("frame-ancestors"), "frame-ancestors is only honoured as a header");
  assert.ok(headers.includes("frame-ancestors 'none'"));
  assert.ok(headers.includes("X-Content-Type-Options"));
  assert.ok(headers.includes("Referrer-Policy"));
});

test("the web package has no third-party dependencies", () => {
  const pkg = JSON.parse(read("package.json"));
  assert.equal(pkg.type, "module");
  const deps = Object.keys(pkg.dependencies ?? {});
  assert.deepEqual(deps, [], "the demo must stay dependency-free (runtime comes from the pinned CDN)");
});
