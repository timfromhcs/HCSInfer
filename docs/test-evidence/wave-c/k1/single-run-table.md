| kernel | bucket (m n k) | HCS wall ms | upstream GGML ms | upstream / HCS | CPU baseline x | HCS / upstream spread | status |
|---|---|---|---|---|---|---|---|
| gemm_coopmat_f16 | m128n128k128 | 0.097 | 0.081 | 0.84x | 5.8x | 0.41 / 0.54 | PASS (correct, not promoted) |
| gemm_coopmat_f16 | m256n1024k256 | 0.306 | 0.213 | 0.70x | 42.4x | 0.12 / 0.12 | PASS (promoted, not default-enabled) |
| gemm_coopmat_f16 | m512n2048k512 | 2.423 | 0.773 | 0.32x | 28.4x | 0.06 / 0.08 | PASS (promoted, not default-enabled) |
| gemm_coopmat_f16 | m1024n1024k1024 | 4.518 | 1.801 | 0.40x | 27.6x | 0.10 / 0.06 | PASS (promoted, not default-enabled) |
| gemm_coopmat_f16 | m1024n1024k64 | 0.268 | 0.179 | 0.67x | 26.0x | 0.19 / 0.18 | PASS (promoted, not default-enabled) |
| gemm_coopmat_f16 | m4096n512k512 | 1.895 | 0.623 | 0.33x | 25.2x | 0.14 / 0.12 | PASS (promoted, not default-enabled) |
| gemm_coopmat_f16 | m1024n512k4096 | 3.082 | 1.097 | 0.36x | 43.4x | 0.11 / 0.08 | PASS (promoted, not default-enabled) |
| gemm_coopmat_f16 | m128n4096k4096 | 10.728 | 2.662 | 0.25x | 24.3x | 0.03 / 0.07 | PASS (promoted, not default-enabled) |
| gemm_coopmat_f16 | m512n4096k4096 | 43.965 | 10.607 | 0.24x | 39.4x | 0.09 / 0.09 | PASS (promoted, not default-enabled) |
| gemm_f16_tiled | m128n128k128 | 0.109 | 0.203 | 1.87x | 13.7x | 0.47 / 0.33 | PASS (correct, not promoted) |
| gemm_f16_tiled | m256n1024k256 | 0.443 | 0.215 | 0.49x | 30.0x | 0.16 / 0.19 | PASS (promoted, not default-enabled) |
| gemm_f16_tiled | m512n2048k512 | 3.982 | 0.811 | 0.20x | 23.5x | 0.03 / 0.07 | PASS (promoted, not default-enabled) |
| gemm_f16_tiled | m1024n1024k1024 | 6.419 | 1.292 | 0.20x | 24.4x | 0.03 / 0.06 | PASS (promoted, not default-enabled) |
| gemm_f16_tiled | m1n1024k4096 | 0.637 | 0.306 | 0.48x | 11.0x | 0.12 / 0.18 | PASS (promoted, not default-enabled) |
| gemm_f16_tiled | m1n4096k4096 | - | - | - | - | - | FAIL (oracle tolerance at k>=1024 with unit-range data; see text) |
| gemm_f16_tiled | m1n16384k4096 | 5.301 | 2.433 | 0.46x | 16.9x | 0.03 / 0.04 | PASS (promoted, not default-enabled) |
| gemm_f16_tiled | m1024n1024k64 | 0.569 | 0.283 | 0.50x | 52.2x | 0.42 / 0.14 | PASS (correct, not promoted) |
| gemm_f16_tiled | m4096n512k512 | 2.502 | 0.509 | 0.20x | 31.8x | 0.02 / 0.21 | PASS (promoted, not default-enabled) |
| gemm_f16_tiled | m1024n512k4096 | - | - | - | - | - | FAIL (oracle tolerance at k>=1024 with unit-range data; see text) |
| gemm_f16_tiled | m128n4096k4096 | - | - | - | - | - | FAIL (oracle tolerance at k>=1024 with unit-range data; see text) |
| gemm_f16_tiled | m512n4096k4096 | - | - | - | - | - | FAIL (oracle tolerance at k>=1024 with unit-range data; see text) |
| gemm_i8dot_tiled | m128n128k128 | 0.096 | not_comparable | n/a | 8.9x | 0.55 / 0.00 | PASS (correct, not promoted) |
| gemm_i8dot_tiled | m256n1024k256 | 0.319 | not_comparable | n/a | 31.0x | 0.10 / 0.00 | PASS (promoted, not default-enabled) |
| gemm_i8dot_tiled | m512n2048k512 | 1.901 | not_comparable | n/a | 47.5x | 0.06 / 0.00 | PASS (promoted, not default-enabled) |
| gemm_i8dot_tiled | m1024n1024k1024 | 3.335 | not_comparable | n/a | 40.7x | 0.03 / 0.00 | PASS (promoted, not default-enabled) |
| gemm_i8dot_tiled | m1n1024k4096 | 0.280 | not_comparable | n/a | 8.5x | 0.07 / 0.00 | PASS (promoted, not default-enabled) |
| gemm_i8dot_tiled | m1n4096k4096 | 1.148 | not_comparable | n/a | 15.6x | 0.06 / 0.00 | PASS (promoted, not default-enabled) |
| gemm_i8dot_tiled | m1n16384k4096 | 3.287 | not_comparable | n/a | 16.6x | 0.09 / 0.00 | PASS (promoted, not default-enabled) |
| gemm_i8dot_tiled | m1024n1024k64 | 0.400 | not_comparable | n/a | 78.8x | 0.12 / 0.00 | PASS (promoted, not default-enabled) |
| gemm_i8dot_tiled | m4096n512k512 | 1.494 | not_comparable | n/a | 45.2x | 0.06 / 0.00 | PASS (promoted, not default-enabled) |
| gemm_i8dot_tiled | m1024n512k4096 | 3.067 | not_comparable | n/a | 46.3x | 0.03 / 0.00 | PASS (promoted, not default-enabled) |
| gemm_i8dot_tiled | m128n4096k4096 | 6.630 | not_comparable | n/a | 32.3x | 0.04 / 0.00 | PASS (promoted, not default-enabled) |
| gemm_i8dot_tiled | m512n4096k4096 | 25.426 | not_comparable | n/a | 30.9x | 0.01 / 0.00 | PASS (promoted, not default-enabled) |
| gemm_f32_tiled | m128n128k128 | 0.080 | 0.080 | 0.99x | 6.6x | 0.13 / 0.20 | PASS (promoted, not default-enabled) |
| gemm_f32_tiled | m256n1024k256 | 0.466 | 0.205 | 0.44x | 19.3x | 0.17 / 0.21 | PASS (promoted, not default-enabled) |
| gemm_f32_tiled | m512n2048k512 | 3.182 | 0.870 | 0.27x | 18.2x | 0.07 / 0.07 | PASS (promoted, not default-enabled) |
| gemm_f32_tiled | m1024n1024k1024 | 6.246 | 1.514 | 0.24x | 19.4x | 0.10 / 0.03 | PASS (promoted, not default-enabled) |
| gemm_f32_tiled | m1n1024k4096 | 0.417 | 0.237 | 0.57x | 5.0x | 0.06 / 0.09 | PASS (promoted, not default-enabled) |
| gemm_f32_tiled | m1n4096k4096 | 1.800 | 1.000 | 0.56x | 6.0x | 0.04 / 0.24 | PASS (promoted, not default-enabled) |
| gemm_f32_tiled | m1n16384k4096 | 5.193 | 3.288 | 0.63x | 6.9x | 0.03 / 0.02 | PASS (promoted, not default-enabled) |
| gemm_f32_tiled | m1024n1024k64 | 0.364 | error | n/a | 19.2x | 0.30 / 0.00 | PASS (correct, not promoted) |
| gemm_f32_tiled | m4096n512k512 | 1.998 | 0.509 | 0.25x | 21.6x | 0.31 / 0.10 | PASS (correct, not promoted) |
| gemm_f32_tiled | m1024n512k4096 | - | - | - | - | - | FAIL (oracle tolerance at k>=1024 with unit-range data; see text) |
| gemm_f32_tiled | m128n4096k4096 | - | - | - | - | - | FAIL (oracle tolerance at k>=1024 with unit-range data; see text) |
| gemm_f32_tiled | m512n4096k4096 | - | - | - | - | - | FAIL (oracle tolerance at k>=1024 with unit-range data; see text) |
| softmax_rows | m32n4096k0 | 0.080 | 0.075 | 0.94x | 4.0x | 0.43 / 0.22 | PASS (correct, not promoted) |
| softmax_rows | m256n1024k0 | 0.101 | 0.105 | 1.04x | 6.4x | 0.24 / 0.09 | PASS (promoted, not default-enabled) |
| softmax_rows | m4n32768k0 | 0.147 | 0.123 | 0.84x | 2.1x | 0.18 / 0.22 | PASS (promoted, not default-enabled) |
| softmax_rows | m1024n1024k0 | 0.226 | 0.224 | 0.99x | 4.3x | 0.09 / 0.08 | PASS (promoted, not default-enabled) |
| softmax_rows | m64n16384k0 | 0.293 | 0.225 | 0.77x | 4.1x | 0.22 / 0.18 | PASS (promoted, not default-enabled) |
| layernorm_rows | m32n4096k0 | 0.084 | 0.152 | 1.80x | 3.6x | 0.15 / 0.30 | PASS (promoted, not default-enabled) |
| layernorm_rows | m256n1024k0 | 0.101 | 0.201 | 2.00x | 6.0x | 0.13 / 0.27 | PASS (promoted, not default-enabled) |
| layernorm_rows | m4n32768k0 | 0.172 | 0.153 | 0.89x | 1.7x | 0.37 / 0.26 | PASS (correct, not promoted) |
| layernorm_rows | m1024n1024k0 | 0.220 | 0.487 | 2.22x | 4.0x | 0.23 / 0.13 | PASS (default-enabled) |
| layernorm_rows | m64n16384k0 | 0.317 | 0.466 | 1.47x | 2.7x | 0.16 / 0.08 | PASS (default-enabled) |
| rmsnorm_rows | m32n4096k0 | 0.088 | 0.075 | 0.86x | 2.3x | 0.07 / 0.04 | PASS (promoted, not default-enabled) |
| rmsnorm_rows | m256n1024k0 | 0.095 | 0.094 | 0.98x | 4.2x | 0.22 / 0.17 | PASS (promoted, not default-enabled) |
| rmsnorm_rows | m4n32768k0 | 0.151 | 0.109 | 0.72x | 1.3x | 0.18 / 0.14 | PASS (promoted, not default-enabled) |
| rmsnorm_rows | m1024n1024k0 | 0.212 | 0.225 | 1.06x | 3.0x | 0.18 / 0.26 | PASS (promoted, not default-enabled) |
| rmsnorm_rows | m64n16384k0 | 0.271 | 0.240 | 0.89x | 2.6x | 0.11 / 0.04 | PASS (promoted, not default-enabled) |
| rope_rows | m512n128k0 | 0.061 | 0.060 | 0.98x | 4.9x | 0.12 / 0.40 | PASS (promoted, not default-enabled) |
| rope_rows | m2048n128k0 | 0.087 | 0.092 | 1.05x | 6.3x | 0.08 / 0.17 | PASS (default-enabled) |
| rope_rows | m1n128k0 | 0.075 | 0.050 | 0.66x | 0.0x | 0.25 / 0.34 | PASS (correct, not promoted) |
| rope_rows | m4096n128k0 | 0.123 | 0.136 | 1.10x | 8.0x | 0.14 / 0.45 | PASS (promoted, not default-enabled) |
