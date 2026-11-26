#pragma once
// Conservative kernel planner (AGENTS.md 13). For (op, dtype, shape, device) it returns the fastest path that
// is KNOWN to be correct and worth it on this exact device+driver; everything else stays on the CPU:
//
//   * unknown capability            -> CPU reference
//   * kernel binary / hash unknown  -> CPU reference
//   * no profile evidence           -> CPU reference
//   * evidence for another device, driver or shader hash -> ignored (stale)
//   * promotion needs: correct vs the CPU oracle AND >= 3 bit-identical runs AND >= 1.05x vs the baseline
//     (kernel_profile.hpp: evaluate_promotion)
//   * default-enabled additionally needs a MEASURED upstream GGML Vulkan baseline >= 1.05x; without it the
//     kernel is only a "candidate" and `Plan::kind` stays Reference.
//
// Ported from hcsuda @94a3485 crates/hcsuda-core/src/planner.rs (+ the promotion rule of README.md "rules"),
// see docs/hcsuda-port.md. Pure logic: no Vulkan, no I/O.
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "hcs/hardware/caps.hpp"
#include "hcs/hardware/kernel_profile.hpp"

namespace hcs::hardware {

enum class Op : std::uint8_t { Gemm = 0, Softmax, LayerNorm, RmsNorm, Rope };
const char* to_string(Op o) noexcept;
std::optional<Op> op_from_string(const std::string& s);

enum class PathKind : std::uint8_t { Reference = 0, VulkanScalar, VulkanTiled, VulkanCoopMat, VulkanIntDot };
const char* to_string(PathKind p) noexcept;

// Problem size. Gemm: C[m x n] = A[m x k] B[k x n]. Row ops: m = rows, n = cols (rope: dim), k = 0.
struct Shape {
    std::uint64_t m = 0, n = 0, k = 0;
};

// Power-of-two bucket per dimension ("m256n1024k256"); a profile entry is valid for its bucket.
std::string shape_bucket(const Shape& s);

struct KernelDesc {
    const char* id;       // stable id, also the shader symbol stem
    Op op;
    DType dtype;
    PathKind path;
    const char* shader;   // shaders/llm/<shader>.comp
    const char* summary;
};
const std::vector<KernelDesc>& kernel_catalog();
const KernelDesc* find_kernel(const std::string& id);
// hcsuda-style evidence key "path:op:dtype", e.g. "vulkan_tiled:gemm:f32".
std::string path_key(const KernelDesc& k);

// Capability gate: can this kernel run on this device at all (and, when `shape` is given, for that shape)?
struct Gate {
    bool eligible = false;
    std::string reason;  // why not / what was proven; never empty
};
Gate gate_kernel(const KernelDesc& k, const DeviceCaps& caps, const Shape* shape = nullptr);

// kernel id -> hash of the SPIR-V actually embedded in the binary. A kernel that is missing here is unknown.
using KernelHashes = std::map<std::string, std::string>;

struct PlanPolicy {
    PromotionPolicy promotion;
    // Test/benchmark seam: also use promoted kernels that have no measured upstream baseline. NEVER set by
    // default; the release default is false (AGENTS.md 12.1: profile first, enable only with proof).
    bool allow_unbaselined = false;
};

struct Plan {
    Op op = Op::Gemm;
    DType dtype = DType::F32;
    Shape shape;
    PathKind kind = PathKind::Reference;
    std::string kernel_id;            // empty for Reference
    std::string reason;               // one sentence
    std::string candidate_kernel_id;  // best verified-but-not-default kernel, if any
    std::string candidate_status;     // "" | "unverified" | "verified" | "promoted" | "stale" | ...
};

Plan plan(Op op, DType dtype, const Shape& shape, const DeviceCaps& caps, const KernelProfile& profile,
          const KernelHashes& hashes, const PlanPolicy& policy = {});

}  // namespace hcs::hardware
