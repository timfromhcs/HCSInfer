# Security policy

HCSInfer 1.1.0-rc.1 is a release candidate (no 1.1.0 release has been published). Report vulnerabilities privately to the
repository owner (do not open a public issue with exploit details). Include version, OS build and a minimal
reproducer; redact personal paths and tokens. Expect an acknowledgement, not an SLA: this is a one-maintainer project.

## Design rules (enforced by tests where code exists)

- The API and MCP bind to loopback by default; LAN exposure needs the explicit `--lan` opt-in, and the bearer token is
  mandatory in every mode (`Settings` refuses a non-loopback `bind_host` without `allow_lan`). The API validates `Host`
  and `Origin`, caps body size, JSON depth, concurrency and request time, and never emits CORS headers
  (`docs/api.md`, ADR-007).
- Every user-supplied path passes the approved-root `PathGuard` (rejects `..`, NUL, device namespaces and reserved
  Windows device names, alternate data streams, symlink/junction escapes).
- Model files, images and metadata are untrusted input: sizes/offsets are validated with checked arithmetic before
  allocation. The HCSMODEL parser has mutation tests and a 60 s libFuzzer smoke run (UBSan, no ASan); long or scheduled
  fuzzing is NOT RUN.
- No arbitrary shell/process/filesystem tools in MCP or the API; no secrets, prompts or images in logs; no telemetry.
- The HCS Agent sidecar (ADR-011) is designed as a loopback-only, offline child process started without a shell inside a
  kill-on-close Job Object, with an argv-only command allow-list and the same path guard. A real agent session has not
  been run, so this is verified at the logic/supervision level only (`docs/agent.md`).
- `web/` runs inference in the visitor's browser under a strict CSP; a Hugging Face token is kept only in memory or
  `sessionStorage` and sent only to `huggingface.co` (`web/README.md`, ADR-013).
- Dependencies are pinned (`external/dependency-lock.json`, FetchContent tags). The package script generates a CycloneDX
  SBOM (a purl on every component) and SHA-256 checksums. The release workflow runs an OSV gate (`scripts/osv-gate.ps1`):
  HIGH, CRITICAL and unknown-severity findings fail it unless `osv-allowlist.json` justifies them (justification and expiry
  date are mandatory). Status of the first full run is recorded in `docs/test-evidence/final-v110/w2-release/`; at the time
  of writing it has one unresolved finding (llama.cpp CVE-2026-133710, `hrm_text` loader heap out-of-bounds write, fixed
  upstream in b12556, pin older) and the cpp-httplib pin is older than the fixes for four advisories (three waived because the
  affected code is not used). Treat GGUF files of unknown origin as untrusted until the llama.cpp pin is bumped. Dependency scans were run once (wave G, `docs/test-evidence/wave-g/`: `npm audit` 0 findings, OSV query of the native
  pins which led to the cpp-httplib v0.44.0 bump, `cargo audit` of prime-agent with the findings analysed in
  `docs/licence-audit.md`); scanners for the vendored C libraries and a literal `cargo deny` run are NOT RUN. The
  licence review is `docs/licence-audit.md`.

## Supported versions

Only the latest release candidate / `main`. No stable release has been made; binaries are unsigned (check
`SHA256SUMS.txt`).
