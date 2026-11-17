// Minimal static server for local development of HCS Studio Web.
//
// No dependency, no build step: it serves this directory with the same security
// headers that vercel.json sets, so a local run matches production. Run it with
//   node tools/serve.mjs [port]
// and open http://127.0.0.1:<port>/ .

import { createServer } from "node:http";
import { createReadStream, statSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join, normalize, resolve, sep } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(join(here, ".."));
const port = Number(process.argv[2] || process.env.PORT || 8080);

const MIME = {
  ".html": "text/html; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".mjs": "text/javascript; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".svg": "image/svg+xml",
  ".png": "image/png",
  ".webp": "image/webp",
  ".ico": "image/x-icon",
};

const CSP =
  "default-src 'none'; script-src 'self' 'wasm-unsafe-eval' https://cdn.jsdelivr.net; style-src 'self'; " +
  "img-src 'self' data: blob:; connect-src 'self' https://huggingface.co https://cdn-lfs.huggingface.co " +
  "https://cdn-lfs-us-1.huggingface.co https://cdn.jsdelivr.net; worker-src 'self' blob:; font-src 'none'; " +
  "media-src 'none'; object-src 'none'; frame-src 'none'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'";

const SECURITY_HEADERS = {
  "Content-Security-Policy": CSP,
  "X-Content-Type-Options": "nosniff",
  "Referrer-Policy": "no-referrer",
  "X-Frame-Options": "DENY",
  "Permissions-Policy": "camera=(), microphone=(), geolocation=(), payment=(), usb=(), interest-cohort=()",
  "Cache-Control": "no-store",
};

function safePath(urlPath) {
  const clean = decodeURIComponent(urlPath.split("?")[0].split("#")[0]);
  const relative = normalize(clean === "/" ? "/index.html" : clean).replace(/^([/\\])+/, "");
  const target = resolve(root, relative);
  if (target !== root && !target.startsWith(root + sep)) {
    return null;
  }
  return target;
}

const server = createServer((req, res) => {
  if (req.method !== "GET" && req.method !== "HEAD") {
    res.writeHead(405, { Allow: "GET, HEAD", ...SECURITY_HEADERS });
    res.end();
    return;
  }
  const target = safePath(req.url || "/");
  if (!target) {
    res.writeHead(400, SECURITY_HEADERS);
    res.end("bad path");
    return;
  }
  let stats;
  try {
    stats = statSync(target);
  } catch {
    res.writeHead(404, SECURITY_HEADERS);
    res.end("not found");
    return;
  }
  if (!stats.isFile()) {
    res.writeHead(404, SECURITY_HEADERS);
    res.end("not found");
    return;
  }
  const ext = target.slice(target.lastIndexOf("."));
  res.writeHead(200, { "Content-Type": MIME[ext] || "application/octet-stream", ...SECURITY_HEADERS });
  if (req.method === "HEAD") {
    res.end();
    return;
  }
  createReadStream(target).pipe(res);
});

server.listen(port, "127.0.0.1", () => {
  process.stdout.write(`HCS Studio Web on http://127.0.0.1:${port}/ (serving ${root})\n`);
});
