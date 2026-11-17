# ADR-009: GUI toolkit - embedded local web UI served by the loopback API

- Status: ACCEPTED (2026-11-17, implemented). Accepted on implementation evidence; the open items are listed below.
- Date: 2026-11-17 (proposed and implemented on the same day)

## Context
AGENTS.md M4 requires Chat, Vision, Image Studio, Models, Performance Center and Settings, Simple/Advanced modes, with
every control wired to real runtime functions, a UI that starts even if Vulkan fails, and no heavy dependency that
endangers the single-EXE goal (ADR-008). The control plane, token-authenticated loopback HTTP API, job/stream/cancel
semantics and `/hcs/v1/{runtime,devices,memory,models,jobs}` routes already exist and are tested.

## Options
1. Native toolkit (Qt, wxWidgets, Win32/Dear ImGui): large dependency or custom rendering work, licence review
   (Qt LGPL/commercial), extra static-link risk.
2. Embedded static web UI (HTML/CSS/JS, no framework build step, no CDN) compiled into the binary and served by the
   existing API server at `http://127.0.0.1:<port>/`, opened by a future `HCSInfer ui` command in the default browser
   (or an optional WebView2 host later).
3. Electron/Tauri shell: separate runtime and process, rejected for V1.

## Decision
Option 2. The UI is only a client of the public API, so it cannot bypass the control plane (AGENTS.md 3.2) and
exercises the same auth, path guard and limits as any other client. Requirements:
- assets embedded at build time (no network, no CDN, no remote fonts), served with a strict
  `Content-Security-Policy` and the existing Host/Origin validation; the token reaches the page through the launcher
  (URL fragment or one-time local handoff) and is never logged;
- no inert controls: a control exists only when its API route works; features that return `501`/`unsupported` are shown
  as unavailable with the server's reason;
- progress, device, backend and memory come from `/hcs/v1/*`, not invented by the UI;
- Simple/Advanced split and reset-to-default for every advanced option;
- works with Vulkan failed (CPU-only), shows load/busy/error/OOM states, cancels via `/hcs/v1/jobs/{id}/cancel`.
Prerequisites that must land first: chat image input, `/v1/images/*` and the benchmark route. All three landed
(vision input on chat, image routes and the `benchmark` command); the UI is implemented in `src/gui` and served by
`ApiHandler::handle()` before bearer auth (after the Host/Origin checks). See `docs/gui.md`.

### Implementation status (2026-11-17)

Implemented: `src/gui` (`hcs_gui`, assets embedded by `embed_assets.cmake`), hook in `src/api/http_api.cpp`, CLI
`mcp`/`serve` unchanged, unit tests `tests/unit/gui`, contract tests `tests/contract/api_image` (`ApiUi.*`) and the
e2e `tests/e2e/gui_real/e2e_gui.py`. Open/not yet done: no `HCSInfer ui` launcher command (the URL is printed by
`serve`); GUI not yet run on the Vulkan build; `playwright` interaction phase NOT RUN on this machine; image edit is
not in the UI (route and MCP tool exist).

## Rejected
Native toolkit for V1 (dependency weight, unproven static link); a UI that talks to engines directly.

## Risks
A default browser must exist; localhost web surfaces need DNS-rebinding and CSRF care (Host/Origin checks exist, CSP
and token handoff must be added and tested); image/streaming payloads must stay within body caps; "GUI" in AGENTS.md
may be read as native, so this ADR records the interpretation and is open to reversal by the user.

## Evidence
None yet (NOT RUN). Acceptance is the M4 gate: a non-expert completes load, chat, vision and image generation without a
terminal, verified against real models.
