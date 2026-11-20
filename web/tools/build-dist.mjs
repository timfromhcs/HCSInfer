// Builds web/dist: the exact set of static files to upload to a static host (e.g. Vercel). No bundler, no npm
// dependency: the site is already plain ES modules, so "build" = copy the runtime files, drop tests/tools, write a
// deploy-ready vercel.json (headers only: no buildCommand, the dist IS the output) and a manifest with SHA-256s.
// Nothing is deployed by this script.
//   node tools/build-dist.mjs            # writes web/dist
import { cpSync, mkdirSync, rmSync, readFileSync, writeFileSync, readdirSync, statSync } from "node:fs";
import { createHash } from "node:crypto";
import { fileURLToPath } from "node:url";
import { dirname, join, relative, resolve, sep } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(join(here, ".."));
const dist = join(root, "dist");
if (!dist.startsWith(root + sep)) throw new Error("refusing to write outside web/");

rmSync(dist, { recursive: true, force: true }); // only the generated dist folder
mkdirSync(dist, { recursive: true });
for (const f of ["index.html", "app.css"]) cpSync(join(root, f), join(dist, f));
cpSync(join(root, "js"), join(dist, "js"), { recursive: true });

const vercel = JSON.parse(readFileSync(join(root, "vercel.json"), "utf8"));
delete vercel.buildCommand;      // the upload already is the build output
vercel.outputDirectory = ".";
writeFileSync(join(dist, "vercel.json"), JSON.stringify(vercel, null, 2) + "\n");

const files = [];
(function walk(d) {
  for (const n of readdirSync(d).sort()) {
    const p = join(d, n);
    if (statSync(p).isDirectory()) walk(p);
    else files.push(p);
  }
})(dist);
const entries = files
  .filter((p) => !p.endsWith("manifest.json"))
  .map((p) => ({
    path: relative(dist, p).split(sep).join("/"),
    bytes: statSync(p).size,
    sha256: createHash("sha256").update(readFileSync(p)).digest("hex"),
  }));
const pkg = JSON.parse(readFileSync(join(root, "package.json"), "utf8"));
writeFileSync(join(dist, "manifest.json"), JSON.stringify({ name: pkg.name, version: pkg.version, files: entries }, null, 2) + "\n");

// Sanity: no source map / test / node_modules leaked, CSP header present, entry point exists.
const bad = entries.filter((e) => /(^|\/)(tests?|node_modules|tools)\//.test(e.path) || e.path.endsWith(".map"));
if (bad.length) throw new Error("unexpected files in dist: " + bad.map((b) => b.path).join(", "));
if (!JSON.stringify(vercel).includes("Content-Security-Policy")) throw new Error("dist vercel.json lost the CSP header");
console.log(`web/dist ready: ${entries.length} files, ${entries.reduce((a, e) => a + e.bytes, 0)} bytes`);
