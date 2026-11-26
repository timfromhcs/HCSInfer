Decision over 1 runs on AMD Radeon(TM) Graphics (vk:1002:1681:drv=0x00800161:api=1.4.315), HCS commit 14696380ccb7+dirty.
Rule: promoted iff in ALL 1 runs: correct, 3/3 identical, spread <= 0.25, >= 1.05x vs CPU baseline, >= 1.05x vs upstream GGML Vulkan (ratio = upstream ms / HCS ms)

| kernel | bucket | vs CPU (min) | vs upstream GGML (min..max) | HCS/upstream spread (max) | promoted | first failing reason |
|---|---|---|---|---|---|---|
| gemm_coopmat_f16 | m128n128k128 | 5.81x | 0.84x .. 0.84x | 0.41 / 0.54 | no | HCS timing too noisy in a run (spread 0.410 > 0.25) |
| gemm_coopmat_f16 | m256n1024k256 | 42.43x | 0.69x .. 0.69x | 0.12 / 0.12 | no | slower than / not >= 1.05x faster than the upstream GGML Vulkan operator (ratio min 0.70x, max 0.70x) |
| gemm_coopmat_f16 | m512n2048k512 | 28.40x | 0.32x .. 0.32x | 0.06 / 0.08 | no | slower than / not >= 1.05x faster than the upstream GGML Vulkan operator (ratio min 0.32x, max 0.32x) |
| gemm_coopmat_f16 | m1024n1024k1024 | 27.62x | 0.40x .. 0.40x | 0.10 / 0.06 | no | slower than / not >= 1.05x faster than the upstream GGML Vulkan operator (ratio min 0.40x, max 0.40x) |
| gemm_coopmat_f16 | m1024n1024k64 | 25.99x | 0.67x .. 0.67x | 0.19 / 0.18 | no | slower than / not >= 1.05x faster than the upstream GGML Vulkan operator (ratio min 0.67x, max 0.67x) |
| gemm_coopmat_f16 | m4096n512k512 | 25.21x | 0.33x .. 0.33x | 0.14 / 0.12 | no | slower than / not >= 1.05x faster than the upstream GGML Vulkan operator (ratio min 0.33x, max 0.33x) |
| gemm_coopmat_f16 | m1024n512k4096 | 43.37x | 0.36x .. 0.36x | 0.11 / 0.08 | no | slower than / not >= 1.05x faster than the upstream GGML Vulkan operator (ratio min 0.36x, max 0.36x) |
| gemm_coopmat_f16 | m128n4096k4096 | 24.33x | 0.25x .. 0.25x | 0.03 / 0.07 | no | slower than / not >= 1.05x faster than the upstream GGML Vulkan operator (ratio min 0.25x, max 0.25x) |
| gemm_coopmat_f16 | m512n4096k4096 | 39.45x | 0.24x .. 0.24x | 0.09 / 0.09 | no | slower than / not >= 1.05x faster than the upstream GGML Vulkan operator (ratio min 0.24x, max 0.24x) |
| gemm_f16_tiled | m128n128k128 | 13.72x | 1.87x .. 1.87x | 0.47 / 0.33 | no | HCS timing too noisy in a run (spread 0.475 > 0.25) |
| gemm_f16_tiled | m256n1024k256 | 30.04x | 0.48x .. 0.48x | 0.16 / 0.19 | no | slower than / not >= 1.05x faster than the upstream GGML Vulkan operator (ratio min 0.49x, max 0.49x) |
| gemm_f16_tiled | m512n2048k512 | 23.46x | 0.20x .. 0.20x | 0.03 / 0.07 | no | slower than / not >= 1.05x faster than the upstream GGML Vulkan operator (ratio min 0.20x, max 0.20x) |
| gemm_f16_tiled | m1024n1024k1024 | 24.42x | 0.20x .. 0.20x | 0.04 / 0.06 | no | slower than / not >= 1.05x faster than the upstream GGML Vulkan operator (ratio min 0.20x, max 0.20x) |
| gemm_f16_tiled | m1n1024k4096 | 10.99x | 0.48x .. 0.48x | 0.12 / 0.18 | no | slower than / not >= 1.05x faster than the upstream GGML Vulkan operator (ratio min 0.48x, max 0.48x) |
| gemm_f16_tiled | m1n4096k4096 | 0.00x | n/a | 0.00 / 0.00 | no | incorrect result in a run |
| gemm_f16_tiled | m1n16384k4096 | 16.92x | 0.46x .. 0.46x | 0.03 / 0.04 | no | slower than / not >= 1.05x faster than the upstream GGML Vulkan operator (ratio min 0.46x, max 0.46x) |
| gemm_f16_tiled | m1024n1024k64 | 52.22x | 0.50x .. 0.50x | 0.42 / 0.14 | no | HCS timing too noisy in a run (spread 0.418 > 0.25) |
| gemm_f16_tiled | m4096n512k512 | 31.82x | 0.20x .. 0.20x | 0.02 / 0.21 | no | slower than / not >= 1.05x faster than the upstream GGML Vulkan operator (ratio min 0.20x, max 0.20x) |
| gemm_f16_tiled | m1024n512k4096 | 0.00x | n/a | 0.00 / 0.00 | no | incorrect result in a run |
| gemm_f16_tiled | m128n4096k4096 | 0.00x | n/a | 0.00 / 0.00 | no | incorrect result in a run |
| gemm_f16_tiled | m512n4096k4096 | 0.00x | n/a | 0.00 / 0.00 | no | incorrect result in a run |
| gemm_i8dot_tiled | m128n128k128 | 8.89x | not_comparable | 0.55 / 0.00 | no | upstream baseline not_comparable: no GGML operator computes int8 x int8 -> int32 (closest: Q8_0 mul_mat with f32 activations, different semantics) |
| gemm_i8dot_tiled | m256n1024k256 | 31.05x | not_comparable | 0.10 / 0.00 | no | upstream baseline not_comparable: no GGML operator computes int8 x int8 -> int32 (closest: Q8_0 mul_mat with f32 activations, different semantics) |
| gemm_i8dot_tiled | m512n2048k512 | 47.46x | not_comparable | 0.06 / 0.00 | no | upstream baseline not_comparable: no GGML operator computes int8 x int8 -> int32 (closest: Q8_0 mul_mat with f32 activations, different semantics) |
| gemm_i8dot_tiled | m1024n1024k1024 | 40.67x | not_comparable | 0.03 / 0.00 | no | upstream baseline not_comparable: no GGML operator computes int8 x int8 -> int32 (closest: Q8_0 mul_mat with f32 activations, different semantics) |
| gemm_i8dot_tiled | m1n1024k4096 | 8.52x | not_comparable | 0.07 / 0.00 | no | upstream baseline not_comparable: no GGML operator computes int8 x int8 -> int32 (closest: Q8_0 mul_mat with f32 activations, different semantics) |
| gemm_i8dot_tiled | m1n4096k4096 | 15.57x | not_comparable | 0.06 / 0.00 | no | upstream baseline not_comparable: no GGML operator computes int8 x int8 -> int32 (closest: Q8_0 mul_mat with f32 activations, different semantics) |
| gemm_i8dot_tiled | m1n16384k4096 | 16.58x | not_comparable | 0.09 / 0.00 | no | upstream baseline not_comparable: no GGML operator computes int8 x int8 -> int32 (closest: Q8_0 mul_mat with f32 activations, different semantics) |
| gemm_i8dot_tiled | m1024n1024k64 | 78.76x | not_comparable | 0.12 / 0.00 | no | upstream baseline not_comparable: no GGML operator computes int8 x int8 -> int32 (closest: Q8_0 mul_mat with f32 activations, different semantics) |
| gemm_i8dot_tiled | m4096n512k512 | 45.22x | not_comparable | 0.06 / 0.00 | no | upstream baseline not_comparable: no GGML operator computes int8 x int8 -> int32 (closest: Q8_0 mul_mat with f32 activations, different semantics) |
| gemm_i8dot_tiled | m1024n512k4096 | 46.27x | not_comparable | 0.04 / 0.00 | no | upstream baseline not_comparable: no GGML operator computes int8 x int8 -> int32 (closest: Q8_0 mul_mat with f32 activations, different semantics) |
| gemm_i8dot_tiled | m128n4096k4096 | 32.35x | not_comparable | 0.04 / 0.00 | no | upstream baseline not_comparable: no GGML operator computes int8 x int8 -> int32 (closest: Q8_0 mul_mat with f32 activations, different semantics) |
| gemm_i8dot_tiled | m512n4096k4096 | 30.87x | not_comparable | 0.01 / 0.00 | no | upstream baseline not_comparable: no GGML operator computes int8 x int8 -> int32 (closest: Q8_0 mul_mat with f32 activations, different semantics) |
| gemm_f32_tiled | m128n128k128 | 6.63x | 0.99x .. 0.99x | 0.13 / 0.20 | no | slower than / not >= 1.05x faster than the upstream GGML Vulkan operator (ratio min 0.99x, max 0.99x) |
| gemm_f32_tiled | m256n1024k256 | 19.34x | 0.44x .. 0.44x | 0.17 / 0.21 | no | slower than / not >= 1.05x faster than the upstream GGML Vulkan operator (ratio min 0.44x, max 0.44x) |
| gemm_f32_tiled | m512n2048k512 | 18.17x | 0.27x .. 0.27x | 0.07 / 0.07 | no | slower than / not >= 1.05x faster than the upstream GGML Vulkan operator (ratio min 0.27x, max 0.27x) |
| gemm_f32_tiled | m1024n1024k1024 | 19.38x | 0.24x .. 0.24x | 0.10 / 0.03 | no | slower than / not >= 1.05x faster than the upstream GGML Vulkan operator (ratio min 0.24x, max 0.24x) |
| gemm_f32_tiled | m1n1024k4096 | 5.01x | 0.57x .. 0.57x | 0.06 / 0.09 | no | slower than / not >= 1.05x faster than the upstream GGML Vulkan operator (ratio min 0.57x, max 0.57x) |
| gemm_f32_tiled | m1n4096k4096 | 6.04x | 0.56x .. 0.56x | 0.04 / 0.24 | no | slower than / not >= 1.05x faster than the upstream GGML Vulkan operator (ratio min 0.56x, max 0.56x) |
| gemm_f32_tiled | m1n16384k4096 | 6.94x | 0.63x .. 0.63x | 0.03 / 0.02 | no | slower than / not >= 1.05x faster than the upstream GGML Vulkan operator (ratio min 0.63x, max 0.63x) |
| gemm_f32_tiled | m1024n1024k64 | 19.20x | error | 0.30 / 0.00 | no | upstream baseline error: no upstream mul_mat variant ran: [f32 activations: upstream operator result failed the spot check (error 0.000101 > 0.000100)] |
| gemm_f32_tiled | m4096n512k512 | 21.57x | 0.26x .. 0.26x | 0.31 / 0.10 | no | HCS timing too noisy in a run (spread 0.306 > 0.25) |
| gemm_f32_tiled | m1024n512k4096 | 0.00x | n/a | 0.00 / 0.00 | no | incorrect result in a run |
| gemm_f32_tiled | m128n4096k4096 | 0.00x | n/a | 0.00 / 0.00 | no | incorrect result in a run |
| gemm_f32_tiled | m512n4096k4096 | 0.00x | n/a | 0.00 / 0.00 | no | incorrect result in a run |
| softmax_rows | m32n4096k0 | 3.99x | 0.94x .. 0.94x | 0.43 / 0.22 | no | HCS timing too noisy in a run (spread 0.430 > 0.25) |
| softmax_rows | m256n1024k0 | 6.36x | 1.04x .. 1.04x | 0.24 / 0.09 | no | slower than / not >= 1.05x faster than the upstream GGML Vulkan operator (ratio min 1.04x, max 1.04x) |
| softmax_rows | m4n32768k0 | 2.12x | 0.84x .. 0.84x | 0.18 / 0.21 | no | slower than / not >= 1.05x faster than the upstream GGML Vulkan operator (ratio min 0.84x, max 0.84x) |
| softmax_rows | m1024n1024k0 | 4.32x | 0.99x .. 0.99x | 0.09 / 0.08 | no | slower than / not >= 1.05x faster than the upstream GGML Vulkan operator (ratio min 0.99x, max 0.99x) |
| softmax_rows | m64n16384k0 | 4.09x | 0.77x .. 0.77x | 0.22 / 0.18 | no | slower than / not >= 1.05x faster than the upstream GGML Vulkan operator (ratio min 0.77x, max 0.77x) |
| layernorm_rows | m32n4096k0 | 3.60x | 1.80x .. 1.80x | 0.15 / 0.30 | no | upstream timing too noisy in a run (spread 0.297 > 0.25) |
| layernorm_rows | m256n1024k0 | 6.04x | 2.00x .. 2.00x | 0.13 / 0.27 | no | upstream timing too noisy in a run (spread 0.269 > 0.25) |
| layernorm_rows | m4n32768k0 | 1.73x | 0.89x .. 0.89x | 0.36 / 0.26 | no | HCS timing too noisy in a run (spread 0.365 > 0.25) |
| layernorm_rows | m1024n1024k0 | 3.96x | 2.22x .. 2.22x | 0.23 / 0.13 | no | only 1 run(s): the rule needs 3 whole-suite runs, so nothing is promoted yet (single-run ratio min 2.22x) |
| layernorm_rows | m64n16384k0 | 2.67x | 1.47x .. 1.47x | 0.16 / 0.08 | no | only 1 run(s): the rule needs 3 whole-suite runs, so nothing is promoted yet (single-run ratio min 1.47x) |
| rmsnorm_rows | m32n4096k0 | 2.29x | 0.85x .. 0.85x | 0.07 / 0.04 | no | slower than / not >= 1.05x faster than the upstream GGML Vulkan operator (ratio min 0.86x, max 0.86x) |
| rmsnorm_rows | m256n1024k0 | 4.23x | 0.98x .. 0.98x | 0.21 / 0.17 | no | slower than / not >= 1.05x faster than the upstream GGML Vulkan operator (ratio min 0.98x, max 0.98x) |
| rmsnorm_rows | m4n32768k0 | 1.31x | 0.72x .. 0.72x | 0.18 / 0.14 | no | slower than / not >= 1.05x faster than the upstream GGML Vulkan operator (ratio min 0.72x, max 0.72x) |
| rmsnorm_rows | m1024n1024k0 | 2.99x | 1.06x .. 1.06x | 0.18 / 0.26 | no | upstream timing too noisy in a run (spread 0.262 > 0.25) |
| rmsnorm_rows | m64n16384k0 | 2.60x | 0.89x .. 0.89x | 0.11 / 0.04 | no | slower than / not >= 1.05x faster than the upstream GGML Vulkan operator (ratio min 0.89x, max 0.89x) |
| rope_rows | m512n128k0 | 4.88x | 0.98x .. 0.98x | 0.12 / 0.40 | no | upstream timing too noisy in a run (spread 0.397 > 0.25) |
| rope_rows | m2048n128k0 | 6.34x | 1.05x .. 1.05x | 0.08 / 0.17 | no | only 1 run(s): the rule needs 3 whole-suite runs, so nothing is promoted yet (single-run ratio min 1.05x) |
| rope_rows | m1n128k0 | 0.03x | 0.66x .. 0.66x | 0.25 / 0.34 | no | upstream timing too noisy in a run (spread 0.337 > 0.25) |
| rope_rows | m4096n128k0 | 7.99x | 1.10x .. 1.10x | 0.14 / 0.45 | no | upstream timing too noisy in a run (spread 0.446 > 0.25) |

Promoted: 0 of 64 kernel/bucket pairs.

