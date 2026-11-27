Decision over 3 runs on AMD Radeon(TM) Graphics (vk:1002:1681:drv=0x00800161:api=1.4.315), HCS commit 540622875eb7.
Rule: promoted iff in ALL 3 runs: correct, 3/3 identical, spread <= 0.25, >= 1.05x vs CPU baseline, >= 1.05x vs upstream GGML Vulkan (ratio = upstream ms / HCS ms)

| kernel | bucket | vs CPU (min) | vs upstream GGML (min..max) | HCS/upstream spread (max) | promoted | first failing reason |
|---|---|---|---|---|---|---|
| gemm_coopmat_f16 | m128n128k128 | 6.51x | 1.22x .. 1.23x | 0.34 / 0.33 | no | HCS timing too noisy in a run (spread 0.335 > 0.25) |
| gemm_coopmat_f16 | m256n1024k256 | 18.26x | 0.72x .. 0.82x | 0.41 / 0.16 | no | HCS timing too noisy in a run (spread 0.406 > 0.25) |
| gemm_coopmat_f16 | m512n2048k512 | 17.06x | 0.34x .. 0.36x | 0.34 / 0.14 | no | HCS timing too noisy in a run (spread 0.338 > 0.25) |
| gemm_coopmat_f16 | m1024n1024k1024 | 18.35x | 0.26x .. 0.29x | 0.09 / 0.18 | no | slower than / not >= 1.05x faster than the upstream GGML Vulkan operator (ratio min 0.26x, max 0.29x) |
| gemm_coopmat_f16 | m1024n1024k64 | 21.05x | n/a | 0.30 / 0.18 | no | missing in a run (skipped or not run) |
| gemm_coopmat_f16 | m4096n512k512 | 17.94x | error | 0.27 / 0.25 | no | upstream baseline error: no upstream mul_mat variant ran: [f32 activations: ggml_backend_alloc_ctx_tensors failed] [f16 activations: ggml_backend_alloc_ctx_tensors failed] |
| gemm_coopmat_f16 | m1024n512k4096 | 24.91x | error | 0.12 / 0.28 | no | upstream baseline error: no upstream mul_mat variant ran: [f32 activations: ggml_backend_alloc_ctx_tensors failed] [f16 activations: ggml_backend_alloc_ctx_tensors failed] |
| gemm_coopmat_f16 | m128n4096k4096 | 12.05x | error | 0.03 / 0.07 | no | upstream baseline error: no upstream mul_mat variant ran: [f32 activations: ggml_backend_alloc_ctx_tensors failed] [f16 activations: ggml_backend_alloc_ctx_tensors failed] |
| gemm_coopmat_f16 | m512n4096k4096 | 12.92x | error | 0.02 / 0.04 | no | upstream baseline error: no upstream mul_mat variant ran: [f32 activations: ggml_backend_alloc_ctx_tensors failed] [f16 activations: ggml_backend_alloc_ctx_tensors failed] |
| gemm_f16_tiled | m128n128k128 | 8.23x | error | 0.25 / 0.20 | no | upstream baseline error: no upstream mul_mat variant ran: [f32 activations: ggml_backend_alloc_ctx_tensors failed] [f16 activations: ggml_backend_alloc_ctx_tensors failed] |
| gemm_f16_tiled | m256n1024k256 | 12.81x | error | 0.28 / 0.30 | no | upstream baseline error: no upstream mul_mat variant ran: [f32 activations: ggml_backend_alloc_ctx_tensors failed] [f16 activations: ggml_backend_alloc_ctx_tensors failed] |
| gemm_f16_tiled | m512n2048k512 | 10.04x | error | 0.30 / 0.17 | no | upstream baseline error: no upstream mul_mat variant ran: [f32 activations: ggml_backend_alloc_ctx_tensors failed] [f16 activations: ggml_backend_alloc_ctx_tensors failed] |
| gemm_f16_tiled | m1024n1024k1024 | 14.42x | error | 0.49 / 0.18 | no | upstream baseline error: no upstream mul_mat variant ran: [f32 activations: ggml_backend_alloc_ctx_tensors failed] [f16 activations: ggml_backend_alloc_ctx_tensors failed] |
| gemm_f16_tiled | m1n1024k4096 | 2.43x | error | 0.12 / 0.18 | no | upstream baseline error: no upstream mul_mat variant ran: [f32 activations: ggml_backend_alloc_ctx_tensors failed] [f16 activations: ggml_backend_alloc_ctx_tensors failed] |
| gemm_f16_tiled | m1n4096k4096 | 4.90x | error | 0.07 / 0.07 | no | upstream baseline error: no upstream mul_mat variant ran: [f32 activations: ggml_backend_alloc_ctx_tensors failed] [f16 activations: ggml_backend_alloc_ctx_tensors failed] |
| gemm_f16_tiled | m1n16384k4096 | 4.23x | error | 0.05 / 0.05 | no | upstream baseline error: no upstream mul_mat variant ran: [f32 activations: ggml_backend_alloc_ctx_tensors failed] [f16 activations: ggml_backend_alloc_ctx_tensors failed] |
| gemm_f16_tiled | m1024n1024k64 | 10.27x | error | 0.25 / 0.22 | no | upstream baseline error: no upstream mul_mat variant ran: [f32 activations: ggml_backend_alloc_ctx_tensors failed] [f16 activations: ggml_backend_alloc_ctx_tensors failed] |
| gemm_f16_tiled | m4096n512k512 | 14.55x | error | 0.39 / 0.23 | no | upstream baseline error: no upstream mul_mat variant ran: [f32 activations: ggml_backend_alloc_ctx_tensors failed] [f16 activations: ggml_backend_alloc_ctx_tensors failed] |
| gemm_f16_tiled | m1024n512k4096 | 15.02x | error | 0.09 / 0.29 | no | upstream baseline error: no upstream mul_mat variant ran: [f32 activations: ggml_backend_alloc_ctx_tensors failed] [f16 activations: ggml_backend_alloc_ctx_tensors failed] |
| gemm_f16_tiled | m128n4096k4096 | 13.24x | error | 0.04 / 0.16 | no | upstream baseline error: no upstream mul_mat variant ran: [f32 activations: ggml_backend_alloc_ctx_tensors failed] [f16 activations: ggml_backend_alloc_ctx_tensors failed] |
| gemm_f16_tiled | m512n4096k4096 | 14.20x | error | 0.03 / 0.04 | no | upstream baseline error: no upstream mul_mat variant ran: [f32 activations: ggml_backend_alloc_ctx_tensors failed] [f16 activations: ggml_backend_alloc_ctx_tensors failed] |
| gemm_i8dot_tiled | m128n128k128 | 12.31x | not_comparable | 0.34 / 0.00 | no | upstream baseline not_comparable: no GGML operator computes int8 x int8 -> int32 (closest: Q8_0 mul_mat with f32 activations, different semantics) |
| gemm_i8dot_tiled | m256n1024k256 | 27.30x | not_comparable | 0.14 / 0.00 | no | upstream baseline not_comparable: no GGML operator computes int8 x int8 -> int32 (closest: Q8_0 mul_mat with f32 activations, different semantics) |
| gemm_i8dot_tiled | m512n2048k512 | 35.51x | not_comparable | 0.08 / 0.00 | no | upstream baseline not_comparable: no GGML operator computes int8 x int8 -> int32 (closest: Q8_0 mul_mat with f32 activations, different semantics) |
| gemm_i8dot_tiled | m1024n1024k1024 | 37.17x | not_comparable | 0.07 / 0.00 | no | upstream baseline not_comparable: no GGML operator computes int8 x int8 -> int32 (closest: Q8_0 mul_mat with f32 activations, different semantics) |
| gemm_i8dot_tiled | m1n1024k4096 | 8.66x | not_comparable | 0.13 / 0.00 | no | upstream baseline not_comparable: no GGML operator computes int8 x int8 -> int32 (closest: Q8_0 mul_mat with f32 activations, different semantics) |
| gemm_i8dot_tiled | m1n4096k4096 | 13.01x | not_comparable | 0.09 / 0.00 | no | upstream baseline not_comparable: no GGML operator computes int8 x int8 -> int32 (closest: Q8_0 mul_mat with f32 activations, different semantics) |
| gemm_i8dot_tiled | m1n16384k4096 | 15.17x | not_comparable | 0.23 / 0.00 | no | upstream baseline not_comparable: no GGML operator computes int8 x int8 -> int32 (closest: Q8_0 mul_mat with f32 activations, different semantics) |
| gemm_i8dot_tiled | m1024n1024k64 | 26.89x | not_comparable | 0.14 / 0.00 | no | upstream baseline not_comparable: no GGML operator computes int8 x int8 -> int32 (closest: Q8_0 mul_mat with f32 activations, different semantics) |
| gemm_i8dot_tiled | m4096n512k512 | 33.99x | not_comparable | 0.13 / 0.00 | no | upstream baseline not_comparable: no GGML operator computes int8 x int8 -> int32 (closest: Q8_0 mul_mat with f32 activations, different semantics) |
| gemm_i8dot_tiled | m1024n512k4096 | 38.71x | not_comparable | 0.04 / 0.00 | no | upstream baseline not_comparable: no GGML operator computes int8 x int8 -> int32 (closest: Q8_0 mul_mat with f32 activations, different semantics) |
| gemm_i8dot_tiled | m128n4096k4096 | 39.12x | not_comparable | 0.04 / 0.00 | no | upstream baseline not_comparable: no GGML operator computes int8 x int8 -> int32 (closest: Q8_0 mul_mat with f32 activations, different semantics) |
| gemm_i8dot_tiled | m512n4096k4096 | 41.67x | not_comparable | 0.03 / 0.00 | no | upstream baseline not_comparable: no GGML operator computes int8 x int8 -> int32 (closest: Q8_0 mul_mat with f32 activations, different semantics) |
| gemm_f32_tiled | m128n128k128 | 7.99x | error | 0.44 / 0.27 | no | upstream baseline error: no upstream mul_mat variant ran: [f32 activations: ggml_backend_alloc_ctx_tensors failed] |
| gemm_f32_tiled | m256n1024k256 | 15.62x | error | 0.31 / 0.16 | no | upstream baseline error: no upstream mul_mat variant ran: [f32 activations: ggml_backend_alloc_ctx_tensors failed] |
| gemm_f32_tiled | m512n2048k512 | 11.32x | error | 0.16 / 0.11 | no | upstream baseline error: no upstream mul_mat variant ran: [f32 activations: ggml_backend_alloc_ctx_tensors failed] |
| gemm_f32_tiled | m1024n1024k1024 | 11.72x | error | 0.05 / 0.15 | no | upstream baseline error: no upstream mul_mat variant ran: [f32 activations: ggml_backend_alloc_ctx_tensors failed] |
| gemm_f32_tiled | m1n1024k4096 | 2.51x | error | 0.09 / 0.16 | no | upstream baseline error: no upstream mul_mat variant ran: [f32 activations: ggml_backend_alloc_ctx_tensors failed] |
| gemm_f32_tiled | m1n4096k4096 | 3.85x | error | 0.28 / 0.08 | no | upstream baseline error: no upstream mul_mat variant ran: [f32 activations: ggml_backend_alloc_ctx_tensors failed] |
| gemm_f32_tiled | m1n16384k4096 | 4.65x | error | 0.11 / 0.14 | no | upstream baseline error: no upstream mul_mat variant ran: [f32 activations: ggml_backend_alloc_ctx_tensors failed] |
| gemm_f32_tiled | m1024n1024k64 | 11.65x | error | 0.15 / 0.00 | no | upstream baseline error: no upstream mul_mat variant ran: [f32 activations: upstream operator result failed the spot check (error 0.000101 > 0.000100)] |
| gemm_f32_tiled | m4096n512k512 | 14.10x | error | 0.35 / 0.45 | no | upstream baseline error: no upstream mul_mat variant ran: [f32 activations: ggml_backend_alloc_ctx_tensors failed] |
| gemm_f32_tiled | m1024n512k4096 | 13.15x | error | 0.05 / 0.25 | no | upstream baseline error: no upstream mul_mat variant ran: [f32 activations: ggml_backend_alloc_ctx_tensors failed] |
| gemm_f32_tiled | m128n4096k4096 | 10.21x | error | 0.04 / 0.05 | no | upstream baseline error: no upstream mul_mat variant ran: [f32 activations: ggml_backend_alloc_ctx_tensors failed] |
| gemm_f32_tiled | m512n4096k4096 | 11.06x | error | 0.03 / 0.03 | no | upstream baseline error: no upstream mul_mat variant ran: [f32 activations: ggml_backend_alloc_ctx_tensors failed] |
| softmax_rows | m32n4096k0 | 4.05x | error | 0.40 / 0.20 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| softmax_rows | m256n1024k0 | 6.75x | error | 0.25 / 0.17 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| softmax_rows | m4n32768k0 | 2.37x | error | 0.17 / 0.26 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| softmax_rows | m1024n1024k0 | 6.73x | error | 0.17 / 0.14 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| softmax_rows | m64n16384k0 | 4.55x | error | 0.14 / 0.17 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| softmax_rows | m1n4096k0 | 0.18x | error | 0.04 / 0.18 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| softmax_rows | m512n4096k0 | 4.80x | error | 0.20 / 0.11 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| softmax_rows | m128n1024k0 | 2.34x | error | 0.24 / 0.31 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| softmax_rows | m1024n2048k0 | 6.47x | error | 0.36 / 0.20 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| softmax_rows | m4096n512k0 | 4.91x | error | 0.13 / 0.09 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| layernorm_rows | m32n4096k0 | 3.45x | error | 0.25 / 0.19 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| layernorm_rows | m256n1024k0 | 5.91x | error | 0.22 / 0.11 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| layernorm_rows | m4n32768k0 | 1.72x | error | 0.16 / 0.17 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| layernorm_rows | m1024n1024k0 | 4.01x | error | 0.23 / 0.06 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| layernorm_rows | m64n16384k0 | 2.74x | error | 0.15 / 0.17 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| layernorm_rows | m1n4096k0 | 0.14x | error | 0.30 / 0.14 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| layernorm_rows | m512n4096k0 | 3.58x | error | 0.15 / 0.10 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| layernorm_rows | m128n1024k0 | 1.89x | error | 0.34 / 0.23 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| layernorm_rows | m1024n2048k0 | 4.19x | error | 0.27 / 0.21 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| layernorm_rows | m4096n512k0 | 4.91x | error | 0.21 / 0.08 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| rmsnorm_rows | m32n4096k0 | 2.22x | error | 1.00 / 0.15 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| rmsnorm_rows | m256n1024k0 | 3.64x | error | 0.39 / 0.19 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| rmsnorm_rows | m4n32768k0 | 1.23x | error | 0.27 / 0.15 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| rmsnorm_rows | m1024n1024k0 | 4.68x | error | 0.22 / 0.23 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| rmsnorm_rows | m64n16384k0 | 2.33x | error | 0.18 / 0.14 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| rmsnorm_rows | m1n4096k0 | 0.08x | error | 0.44 / 0.18 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| rmsnorm_rows | m512n4096k0 | 3.21x | error | 0.08 / 0.09 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| rmsnorm_rows | m128n1024k0 | 1.35x | error | 0.21 / 0.27 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| rmsnorm_rows | m1024n2048k0 | 3.48x | error | 0.15 / 0.10 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| rmsnorm_rows | m4096n512k0 | 3.34x | error | 0.12 / 0.17 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| rope_rows | m512n128k0 | 4.40x | error | 0.28 / 0.26 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| rope_rows | m2048n128k0 | 8.34x | error | 0.27 / 0.17 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| rope_rows | m1n128k0 | 0.03x | error | 0.48 / 0.31 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |
| rope_rows | m4096n128k0 | 8.69x | error | 0.20 / 0.11 | no | upstream baseline error: ggml_backend_alloc_ctx_tensors failed |

Promoted: 0 of 79 kernel/bucket pairs.

