# Security policy

HCSInfer is pre-alpha. Report vulnerabilities privately to the repository owner (do not open a public
issue with exploit details). Include version, OS build and a minimal reproducer; redact personal paths.

## Design rules (enforced by tests where code exists)

- API/MCP bind to loopback by default; LAN exposure needs explicit opt-in plus authentication
  (`Settings` already refuses a non-loopback `bind_host` without `allow_lan`).
- Every user-supplied path passes the approved-root `PathGuard` (rejects `..`, NUL, device namespaces,
  alternate data streams, symlink/junction escapes).
- Model files, images and metadata are untrusted input: sizes/offsets are validated with checked arithmetic
  before allocation; parsers will be fuzzed (M6/M10).
- No arbitrary shell/process/filesystem tools in MCP; no secrets, prompts or images in logs.
- Dependencies are pinned; release builds will ship an SBOM and checksums (M10).

## Supported versions

Only the current `main`. No release has been made.
