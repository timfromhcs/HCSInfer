# S1 evidence: correctness of the unpushed wave A/B/C features (2026-11-27)

Branch `release/v1.1-stable`, commit base `daf4c3e`. Binaries: `build/windows-msvc-release/bin/HCSInfer.exe` (CPU)
and `build/windows-msvc-release-vulkan/bin/HCSInfer.exe` (Vulkan). All real-model work was serialized under
`D:\HCSScratch\realmodel.lock` for the CTest runs; the manual renders below were run one at a time with temp/cache
directories on D: via `scripts/env-d-e.ps1`. No model files are in the repository.

| Gate | Status | Evidence |
|---|---|---|
| Real image LoRA (LCM-LoRA SD1.5, `lcm-sd15`) | **PASS** | `lora-baseline.txt`, `lora-real.txt`, `lora-compare.json`. Same model/seed/prompt/size/steps with and without the LoRA. The LoRA run reports `{"id":"lcm-sd15","weight":1.0}`; images differ (PSNR 11.50 dB, SSIM 0.64, 98.8% of pixels differ >8), i.e. the weights were actually applied. Both 512x512 PNGs were **viewed**: the baseline is a realistic apple, the LoRA result is a coherent high-saturation stylized apple. Times: 89.2 s baseline vs 149.7 s with LoRA (CPU); output SHA-256 in `lora-compare.json`. |
| Real 4K ONNX upscale (Real-ESRGAN x4) | **PASS** | `upscale-real.txt`, `upscale-dimensions.txt`. 512x512 -> **3840x3840**, 4 tiles, 73.3 s, total command 165.1 s; provider honestly reported as `CPU (DirectML unavailable: this onnxruntime.dll does not export the DirectML provider)`. The 3840x3840 PNG was **viewed**: no visible tile seams, coherent upscale. `onnxruntime.dll` + `onnxruntime_providers_shared.dll` were copied into `D:\HCSModels\upscale\` from the local ONNX Runtime package (outside the repo); no DLL is shipped by HCSInfer. |
| Real `HCSInfer tune --suite quick` on the 680M | **PASS** | `tune-quick.json` (exit 0): device id 0x1681, driver 26.6.1, Vulkan 1.4.315, subgroup 64; 10 kernel/shape results, all correct, 3 identical runs each; promoted but **default-enabled: none** because the in-process upstream GGML Vulkan baseline is `NOT RUN`. The f32 GEMM bucket was rejected for timing noise (spread 0.284 > 0.250) rather than promoted. Provenance caveat: the Vulkan EXE was configured at `6bfe340+dirty`; S4 rebuilds static EXEs from the final clean tree. |
| Distinct-model merge with real weights | **BLOCKED** | `merge-quantized-blocked.txt`: SD1.5 Q8_0 + SD-Turbo Q8_0 fail with a clean `invalid_argument` (1131 vs 1323 tensors) - no same-architecture pair is available locally. `merge-quantized-self.txt`: self-merge of the quantized SD1.5 GGUF is refused with `unsupported` (exit 10), proving the float-only guard. A real merged model cannot be produced without downloading a suitable same-architecture float pair (owner approval required). |
| New control-plane features (LoRA library, upscale, merge math, admission controller, vault prefetch, CLI load/unload/status, HCSUDA planner/kernels, image profiles, C:-guard) | **PASS (tests)** | S0 CPU CTest 1054/1054 and Vulkan CTest 1085/1085 in `docs/test-evidence/final-s0/` cover their unit/contract/integration/e2e tests; no assertion was weakened. Features needing external assets stay honest: real merge, real training, real `prime-agent` session, real WebGPU (NOT RUN/BLOCKED). |

## Not claimed

No LPIPS/CLIP score was produced for the LoRA/upscale comparison (SSIM/PSNR and visual inspection only), no DirectML
execution (the installed ORT build has no DirectML export), no GPU-based upscale, and no default-enabled HCS kernel.
