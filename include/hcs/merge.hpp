#pragma once
// Model merging (`HCSInfer merge`): linear, SLERP, TIES, DARE and LoRA-into-base on safetensors or GGUF models.
//
// Scope and limits (V1.1):
//  * Tensor-wise and streaming. No two models are ever fully in memory: tensors are read in bounded chunks;
//    only TIES needs one whole-tensor magnitude buffer per tensor (4 bytes/element, checked against
//    Options::max_ram_bytes, fail-closed). Peak RAM is therefore ~ (models + 1) * chunk + largest TIES tensor.
//  * Float tensors only: F32, F16, BF16 (safetensors) and F32, F16, BF16 (GGUF). A GGUF containing any other
//    tensor type (Q4_K, Q8_0, ...) is rejected (Unsupported): quantized formats are NOT merged in V1.1.
//    Non-float safetensors tensors (e.g. int64 position ids) are copied from the base unchanged.
//  * All inputs must share one container format (safetensors+safetensors or GGUF+GGUF) and identical tensor
//    names and shapes. Any mismatch fails before a single output byte is written.
//  * Output: base dtype per tensor; GGUF output is a byte copy of the base header/padding with merged tensor
//    data, safetensors output re-emits the base header. Written to "<out>.partial" then renamed; the output must
//    lie inside an approved root and never on drive C:.
//  * Arithmetic is done in double per element and rounded once to the output dtype.
//
// Method definitions (w_i = per-model weights, all finite and > 0):
//   linear  out = sum(w_i * m_i) / sum(w_i)
//   slerp   exactly two models, t in [0,1]; per tensor: a = m_0, b = m_1, d = <a/|a|, b/|b|>;
//           |d| > 0.9995 (or a zero norm) => lerp (1-t)a + t b; else theta = acos(d),
//           out = sin((1-t)theta)/sin(theta) * a + sin(t theta)/sin(theta) * b
//   ties    base + N task models (TIES-merging, Yadav et al.): delta_i = m_i - base; keep the top `density`
//           fraction of |delta_i| (>= the k-th largest, k = max(1, ceil(density*n))); weighted_i = w_i *
//           trimmed_i; elected = sign(sum weighted_i); disjoint mean over the entries whose sign equals the
//           elected sign: merged = sum(weighted_i * [sign(weighted_i) == elected]) / sum(w_i * [...]); out =
//           base + lambda * merged
//   dare    base + N task models (DARE-linear, Yu et al.): each delta_i element is dropped with probability p
//           and survivors are rescaled by 1/(1-p); out = base + lambda * sum(w_i * dare_i). The drop mask is a
//           deterministic splitmix64 stream keyed by (seed, tensor name, model index, element index), so a merge
//           is reproducible and testable against an independent implementation.
//   lora    base + weight * (alpha / rank) * (B @ A) for every tensor the LoRA targets (see lora naming below)
//
// LoRA naming understood (target tensor in the base): PEFT `<module>.lora_A.weight` / `.lora_B.weight`
// (optionally prefixed `base_model.model.`, optionally `.default`), kohya `<module>.lora_down.weight` / `.lora_up.weight`
// (+ optional scalar `<module>.alpha`) - both target `<module>.weight` - and llama.cpp style `<tensor>.lora_a` / `.lora_b`
// targeting `<tensor>`. kohya `lora_unet_...` underscore names cannot be mapped to base tensor names and are
// rejected. Every LoRA pair must match a base tensor of shape [out, in]; unmatched pairs are an error.
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "hcs/job_queue.hpp"
#include "hcs/path_guard.hpp"
#include "hcs/result.hpp"

namespace hcs::merge {

enum class Method { Linear, Slerp, Ties, Dare, Lora };
const char* to_string(Method m) noexcept;
Result<Method> parse_method(std::string_view s);

// --- pure math on float buffers (exposed for tests and reuse) -------------------------------------------------

std::uint64_t fnv1a64(std::string_view s) noexcept;
// Uniform double in [0,1) for element `index` of model `model_index` of tensor `tensor_name`.
double dare_uniform(std::uint64_t seed, std::uint64_t tensor_key, std::uint32_t model_index, std::uint64_t index) noexcept;

float half_to_float(std::uint16_t h) noexcept;
std::uint16_t float_to_half(float f) noexcept;  // round to nearest even
float bf16_to_float(std::uint16_t b) noexcept;
std::uint16_t float_to_bf16(float f) noexcept;  // round to nearest even

// out[i] = sum_k w[k]*in[k][i] / sum(w)
void linear_chunk(const std::vector<const float*>& in, const std::vector<double>& w, std::size_t n, float* out);
// Partial sums for SLERP (accumulated across chunks).
struct SlerpStats {
    double dot = 0.0, na = 0.0, nb = 0.0;
};
void slerp_accumulate(const float* a, const float* b, std::size_t n, SlerpStats& s);
struct SlerpCoeffs {
    double ca = 1.0, cb = 0.0;
};
SlerpCoeffs slerp_coeffs(const SlerpStats& s, double t);
void slerp_apply(const float* a, const float* b, std::size_t n, const SlerpCoeffs& c, float* out);
// k-th largest |m_i - base| threshold of a whole tensor (k = max(1, ceil(density * n))). `scratch` is resized to n.
float ties_threshold(const float* model, const float* base, std::size_t n, double density, std::vector<float>& scratch);
// Same selection on a prepared buffer of |delta| values (permutes it): the k-th largest, k = max(1, ceil(density*n)).
float ties_select(std::vector<float>& abs_deltas, double density);
void ties_chunk(const float* base, const std::vector<const float*>& models, const std::vector<double>& w,
                const std::vector<float>& thresholds, double lambda, std::size_t n, float* out);
void dare_chunk(const float* base, const std::vector<const float*>& models, const std::vector<double>& w,
                double drop_rate, std::uint64_t seed, std::uint64_t tensor_key, std::uint64_t first_index,
                double lambda, std::size_t n, float* out);
// out[row][c] = base[row][c] + scale * sum_k B[row][k] * A[k][c]; rows x cols base block, B rows x rank, A rank x cols.
void lora_rows(const float* base, const float* b_rows, const float* a, std::size_t rows, std::size_t cols,
               std::size_t rank, double scale, float* out);

// --- file level ---------------------------------------------------------------------------------------------

struct Params {
    Method method = Method::Linear;
    std::vector<double> weights;  // one per model (empty => equal weights); LoRA: weights[0] is the LoRA strength
    double t = 0.5;               // slerp
    double density = 0.5;         // ties: fraction of each task vector kept, (0,1]
    double lambda = 1.0;          // ties / dare: scale of the merged task vector
    double drop_rate = 0.5;       // dare: p in [0,1)
    std::uint64_t seed = 0;       // dare
    double lora_alpha = 0.0;      // LoRA: alpha when the file has none; 0 => alpha = rank (scale 1)
};

struct Request {
    std::filesystem::path base;
    std::vector<std::filesystem::path> models;  // task models (linear/slerp/ties/dare) or exactly one LoRA file
    std::filesystem::path out;
    Params params;
    std::uint64_t max_ram_bytes = 2ull << 30;  // cap for the largest transient buffer
    std::uint64_t chunk_elements = 4u << 20;   // floats per streaming chunk per input
    bool overwrite = false;
    bool write_manifest = true;  // <out>.merge.json with method, params, input sizes and the output SHA-256
    CancellationToken token;
    std::function<void(double, const std::string&)> progress;  // fraction 0..1, current tensor
};

struct Report {
    std::string format;  // "safetensors" | "gguf"
    std::string method;
    std::uint64_t tensors_total = 0;
    std::uint64_t tensors_merged = 0;
    std::uint64_t tensors_copied = 0;  // non-float tensors and tensors the method leaves unchanged
    std::uint64_t bytes_written = 0;
    std::uint64_t peak_buffer_bytes = 0;  // largest sum of live streaming buffers (accounted, not measured)
    double seconds = 0.0;
    std::string output_sha256;
    std::filesystem::path output;
    std::filesystem::path manifest;
};

// Validates every path through `guard` (approved roots), rejects drive C:, validates the inputs and runs the
// merge. Fail-closed: nothing is written unless validation passed, and a failed or cancelled run leaves no output.
Result<Report> run_merge(const Request& req, const PathGuard& guard);

// The output-path rule on its own (used by the CLI before it prints a plan): inside an approved root, not on C:.
Result<std::filesystem::path> validate_merge_output(const std::filesystem::path& out, const PathGuard& guard);

}  // namespace hcs::merge
