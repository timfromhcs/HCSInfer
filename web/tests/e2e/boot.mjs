// Headless boot check for HCS Studio Web.
//
// Proves what a headless browser CAN prove (AGENTS.md 26.7): the static server
// answers with the security headers, the page loads, the ES modules execute and
// the capability probe runs. Real WebGPU inference is NOT exercised here (no
// WebGPU in headless); that stays NOT RUN until a real WebGPU browser is used.
//
//   node tests/e2e/boot.mjs [--evidence <dir>]
//
// Uses the Chrome/Edge already installed on the machine; no dependency.

import { spawn, spawnSync } from "node:child_process";
import { existsSync, mkdirSync, writeFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join, resolve } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(join(here, "..", ".."));
const port = 8123 + Math.floor(Math.random() * 500);
const base = `http://127.0.0.1:${port}/`;

const evidenceIndex = process.argv.indexOf("--evidence");
const evidenceDir = evidenceIndex >= 0 ? process.argv[evidenceIndex + 1] : null;

const results = [];
function check(name, ok, detail = "") {
  results.push({ name, ok, detail });
  process.stdout.write(`${ok ? "PASS" : "FAIL"}  ${name}${detail ? ` - ${detail}` : ""}\n`);
}

function findBrowser() {
  const candidates = [
    process.env.CHROME_PATH,
    "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
    "C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe",
    "C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
    "C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
    "/usr/bin/google-chrome",
    "/usr/bin/chromium",
  ].filter(Boolean);
  return candidates.find((c) => existsSync(c)) || null;
}

async function waitForServer(url) {
  for (let i = 0; i < 60; i += 1) {
    try {
      const res = await fetch(url);
      if (res.ok) {
        return res;
      }
    } catch {
      /* not up yet */
    }
    await new Promise((r) => setTimeout(r, 100));
  }
  throw new Error("static server did not come up");
}

function screenshot(browser, url, outPath) {
  const args = [
    "--headless=new",
    "--disable-gpu",
    "--no-sandbox",
    "--no-first-run",
    "--disable-extensions",
    "--hide-scrollbars",
    "--window-size=1360,1000",
    "--virtual-time-budget=10000",
    `--screenshot=${outPath}`,
    url,
  ];
  spawnSync(browser, args, { encoding: "utf8", timeout: 60000, maxBuffer: 32 * 1024 * 1024 });
  return existsSync(outPath);
}

function dumpDom(browser, url) {
  const args = [
    "--headless=new",
    "--disable-gpu",
    "--no-sandbox",
    "--no-first-run",
    "--disable-extensions",
    "--virtual-time-budget=12000",
    "--dump-dom",
    url,
  ];
  const run = spawnSync(browser, args, { encoding: "utf8", timeout: 60000, maxBuffer: 32 * 1024 * 1024 });
  return (run.stdout || "") + (run.stderr || "");
}

const server = spawn(process.execPath, [join(root, "tools", "serve.mjs"), String(port)], { stdio: "ignore" });
let exitCode = 0;

try {
  const indexRes = await waitForServer(base);
  check("server responds on 127.0.0.1", indexRes.ok, `HTTP ${indexRes.status}`);
  const csp = indexRes.headers.get("content-security-policy") || "";
  check("Content-Security-Policy header present", csp.includes("default-src 'none'"), csp.slice(0, 40) + "...");
  check("X-Content-Type-Options header present", indexRes.headers.get("x-content-type-options") === "nosniff");
  check("frame-ancestors denied", csp.includes("frame-ancestors 'none'"));

  const html = await indexRes.text();
  check("index.html has the app shell", html.includes('id="chat-log"') && html.includes("importmap"));

  const cssRes = await fetch(`${base}app.css`);
  check("app.css served as text/css", (cssRes.headers.get("content-type") || "").startsWith("text/css"), cssRes.headers.get("content-type") || "");

  const jsRes = await fetch(`${base}js/app.js`);
  check("js/app.js served as javascript", (jsRes.headers.get("content-type") || "").includes("javascript"), jsRes.headers.get("content-type") || "");

  // Real containment test: an encoded path that would escape the web root if the
  // server did not normalise and re-check. AGENTS.md lives two levels above it.
  const traversalUrl = `${base}%2e%2e%2f%2e%2e%2fAGENTS.md`;
  const traversal = await fetch(traversalUrl);
  const traversalBody = await traversal.text();
  check(
    "path traversal is contained (cannot read outside the web root)",
    traversal.status !== 200 && !traversalBody.includes("# HCSInfer"),
    `HTTP ${traversal.status}`,
  );

  const browser = findBrowser();
  if (!browser) {
    check("headless browser available", false, "no Chrome/Edge found (NOT RUN)");
    exitCode = 0; // honest: cannot run, not a product failure
  } else {
    const dom = dumpDom(browser, base);
    check("headless browser loaded a DOM", dom.includes("<!doctype html") || dom.includes("<html"), `browser ${browser}`);
    check("app shell rendered (tabs present)", dom.includes('id="tabs"') && dom.includes("panel-chat"));
    const pill = /id="device-pill"[^>]*>([^<]*)</.exec(dom);
    const pillText = pill ? pill[1].trim() : "";
    const probed = /WebGPU \(GPU\)|WebAssembly \(CPU\)|unavailable/.test(dom);
    check("capability probe ran (device pill updated)", probed, pillText ? `detected: ${pillText}` : "device pill still probing");
    check("chat model options populated", dom.includes("onnx-community/Qwen2.5-0.5B-Instruct"));
    check("no server-side template markers leaked", !dom.includes("{{") && !dom.includes("undefined"));
    if (evidenceDir) {
      mkdirSync(evidenceDir, { recursive: true });
      const shot = resolve(evidenceDir, "web-chat.png").replace(/\\/g, "/");
      check("screenshot captured", screenshot(browser, base, shot), shot);
    }
  }
} catch (err) {
  check("e2e run completed", false, err && err.message ? err.message : String(err));
  exitCode = 1;
} finally {
  server.kill();
}

const failed = results.filter((r) => !r.ok);
const summary = {
  when: new Date().toISOString(),
  status: failed.length === 0 ? "PASS" : "FAIL",
  browser: findBrowser() || null,
  webgpu_inference: "NOT RUN (headless has no WebGPU adapter)",
  results,
};

if (evidenceDir) {
  mkdirSync(evidenceDir, { recursive: true });
  writeFileSync(join(evidenceDir, "web-boot.json"), JSON.stringify(summary, null, 2));
  writeFileSync(
    join(evidenceDir, "web-boot.txt"),
    results.map((r) => `${r.ok ? "PASS" : "FAIL"}  ${r.name}${r.detail ? ` - ${r.detail}` : ""}`).join("\n") + "\n",
  );
}

process.stdout.write(`\n${summary.status}: ${results.length - failed.length}/${results.length} checks\n`);
process.exitCode = failed.length === 0 ? exitCode : 1;
