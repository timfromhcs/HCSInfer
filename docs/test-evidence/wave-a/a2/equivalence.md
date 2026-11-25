## Repeat reproducibility (same configuration, same seed)

| config | runs | distinct sha256 | worst PSNR dB | worst SSIM | max abs diff | max frac px differing | wall s |
|---|---|---|---|---|---|---|---|
| `cpu:cpu` | 3 | 1 | inf | 1.00000 | 0 | 0.0000 | 321, 1105, 258 |
| `cpu:disk` | 3 | 1 | inf | 1.00000 | 0 | 0.0000 | 893, 560, 222 |
| `vulkan0:cpu` | 5 | 2 | 41.53 | 0.99067 | 91 | 0.8982 | 46, 50, 50, 54, 55 |
| `vulkan0:disk` | 3 | 1 | inf | 1.00000 | 0 | 0.0000 | 51, 49, 52 |

## Cross-configuration (first run of each; reference `cpu:cpu`)

| A | B | byte-identical | pixel-identical | PSNR dB | SSIM | max abs diff | mean abs diff | px differing | px diff>8 |
|---|---|---|---|---|---|---|---|---|---|
| `cpu:cpu` | `cpu:disk` | True | True | inf | 1.00000 | 0 | 0.000 | 0.0000 | 0.0000 |
| `cpu:cpu` | `vulkan0:cpu` | False | False | 40.80 | 0.99050 | 81 | 1.353 | 0.9055 | 0.0229 |
| `cpu:cpu` | `vulkan0:disk` | False | False | 39.33 | 0.98902 | 66 | 1.501 | 0.9112 | 0.0373 |
| `cpu:disk` | `vulkan0:cpu` | False | False | 40.80 | 0.99050 | 81 | 1.353 | 0.9055 | 0.0229 |
| `cpu:disk` | `vulkan0:disk` | False | False | 39.33 | 0.98902 | 66 | 1.501 | 0.9112 | 0.0373 |
| `vulkan0:cpu` | `vulkan0:disk` | False | False | 41.53 | 0.99067 | 91 | 1.208 | 0.8982 | 0.0139 |

LPIPS: NOT RUN (needs torch + weights; not installed)
