# Image LoRA library

Status: **Experimental**. The manifest, scanner, compatibility rules, request plumbing (API, MCP, CLI, UI) and the engine
hand-off to the pinned stable-diffusion.cpp (`sd_opt_set_loras`) are implemented and unit/contract tested with synthetic
files. A real render with the LCM-LoRA SD1.5 file is **PASS** (2026-11-27): same seed/model/prompt with and without the
LoRA, PSNR 11.50 dB / SSIM 0.64, both images viewed (`docs/test-evidence/final-s1/`). HCSInfer never downloads a LoRA.

## Folder and manifest

```
<approved model root>/lora/image/manifest.json
<approved model root>/lora/image/<file>.safetensors
```

On this machine: `D:\HCSModels\lora\image\` (created with a README; empty). `HCSInfer serve` and `mcp stdio` scan the first
approved root that contains `lora/image` at start-up (read-only; hashes are only computed on request). The CLI searches
upwards from the model file, or takes `--lora-dir`.

```json
{"schema_version": 1, "loras": [
  {"id": "detail-sd15", "file": "detail-sd15.safetensors", "name": "Detail tweaker",
   "base_family": "sd15", "trigger_words": ["add_detail"], "recommended_weight": 0.7,
   "licence": "see model card", "source": "https://...", "sha256": "<optional 64 hex>"}
]}
```

| Field | Rule |
|---|---|
| `id` | `[A-Za-z0-9._-]`, unique |
| `file` | bare file name in the folder (no separators, no `..`) |
| `base_family` | `sd15`, `sd2`, `sdxl`, `sd3`, `flux1`, `flux3` (anything else: entry `invalid`) |
| `recommended_weight` | `0 < abs(w) <= 2`, default 1 |
| `licence` | required; shown in the UI and `GET /hcs/v1/loras` |
| `sha256` | optional; when scanned with hash verification a mismatch marks the entry `invalid` |

Statuses: `ready`, `invalid` (with `problems`), `missing` (file not found), `unmanifested` (a `.safetensors` file without an
entry; never usable because its base family is unknown). The safetensors header is read (no tensor data) and its tensor names
are an independent plausibility check: a file whose names look like SDXL but whose manifest says `sd15` is `invalid`.

## Using a LoRA

| Surface | How |
|---|---|
| CLI | `HCSInfer image generate --model <id> --prompt "..." --lora detail-sd15:0.7 [--lora-dir D:\HCSModels\lora\image]` (repeatable, max 4) |
| HTTP | `POST /v1/images/generations` with `"loras":[{"id":"detail-sd15","weight":0.7}]`; list with `GET /hcs/v1/loras` |
| MCP | `hcs.image.generate`/`hcs.image.edit` field `loras`; list with `hcs.loras.list` |
| UI | Image Studio, Advanced mode, "LoRAs" (checkbox + weight per usable entry) |

## Safety rules (all enforced in code, all tested)

* Callers send **ids**, never paths. The path comes from the manifest entry and is re-resolved through the `PathGuard` (approved
  root, no `..`, no junction escape) at scan time and again at use time.
* At most 4 LoRAs, weights finite, non-zero, `abs(w) <= 2`, no duplicate ids.
* **Compatibility**: a LoRA is applied only to the family it was trained for. An SD 1.5 LoRA on FLUX.3 (or the reverse) is
  `400 invalid_argument`, message `lora_incompatible: ...`. The control plane checks with the family guessed from the model id;
  the diffusion adapter checks again with the family detected from the loaded weights. If the loaded family cannot be
  recognised the LoRA is rejected (a mismatch cannot be ruled out), not applied.
* No library, or a LoRA request on a build without LoRA support: `unsupported` (HTTP 501, exit code 10), never silently ignored.
* The engine request carries the resolved specs; the result reports `hcs.loras = ["id:weight", ...]`.

## Limits and open items

* Real LoRA render: PASS with `lcm-sd15` on SD1.5 Q8_0 (applied weight reported by the CLI, images viewed and compared);
  other LoRAs and base families remain untested. Whether the pinned stable-diffusion.cpp applies a LoRA to a quantised
  (Q8_0/Q4_K) base model correctly and how much it costs in time/RAM must be measured with a real file.
* Only the base-family names above are known; LoRAs for other architectures are rejected as `invalid`.
