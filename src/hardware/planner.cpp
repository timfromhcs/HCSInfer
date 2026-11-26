// Conservative planner (ported from hcsuda @94a3485 crates/hcsuda-core/src/planner.rs; see docs/hcsuda-port.md).
#include "hcs/hardware/planner.hpp"

#include <algorithm>
#include <cstdio>

namespace hcs::hardware {

const char* to_string(Op o) noexcept {
    switch (o) {
        case Op::Gemm:
            return "gemm";
        case Op::Softmax:
            return "softmax";
        case Op::LayerNorm:
            return "layernorm";
        case Op::RmsNorm:
            return "rmsnorm";
        case Op::Rope:
            return "rope";
    }
    return "unknown";
}

std::optional<Op> op_from_string(const std::string& s) {
    for (Op o : {Op::Gemm, Op::Softmax, Op::LayerNorm, Op::RmsNorm, Op::Rope})
        if (s == to_string(o))
            return o;
    return std::nullopt;
}

const char* to_string(PathKind p) noexcept {
    switch (p) {
        case PathKind::Reference:
            return "reference";
        case PathKind::VulkanScalar:
            return "vulkan_scalar";
        case PathKind::VulkanTiled:
            return "vulkan_tiled";
        case PathKind::VulkanCoopMat:
            return "vulkan_coopmat";
        case PathKind::VulkanIntDot:
            return "vulkan_intdot";
    }
    return "unknown";
}

namespace {
std::uint64_t ceil_pow2(std::uint64_t v) {
    if (v == 0)
        return 0;
    std::uint64_t p = 1;
    while (p < v && p < (1ull << 62))
        p <<= 1;
    return p;
}
}  // namespace

std::string shape_bucket(const Shape& s) {
    return "m" + std::to_string(ceil_pow2(s.m)) + "n" + std::to_string(ceil_pow2(s.n)) + "k" +
           std::to_string(ceil_pow2(s.k));
}

const std::vector<KernelDesc>& kernel_catalog() {
    // Preference order within an (op, dtype): first eligible+promoted wins.
    static const std::vector<KernelDesc> k = {
        {"gemm_coopmat_f16", Op::Gemm, DType::F16, PathKind::VulkanCoopMat, "gemm_coopmat_f16",
         "cooperative-matrix GEMM, f16 inputs, f32 accumulate (16-aligned shapes)"},
        {"gemm_f16_tiled", Op::Gemm, DType::F16, PathKind::VulkanTiled, "gemm_f16_tiled",
         "16x16 shared-memory tiled GEMM, f16 storage, f32 math"},
        {"gemm_i8dot_tiled", Op::Gemm, DType::I8, PathKind::VulkanIntDot, "gemm_i8dot_tiled",
         "16x16 tiled int8 dot-product GEMM, int32 accumulate (k % 4 == 0)"},
        {"gemm_f32_tiled", Op::Gemm, DType::F32, PathKind::VulkanTiled, "gemm_f32_tiled",
         "16x16 shared-memory tiled f32 GEMM"},
        {"softmax_rows", Op::Softmax, DType::F32, PathKind::VulkanTiled, "softmax_rows",
         "row-wise stable softmax, one 256-lane workgroup per row"},
        {"layernorm_rows", Op::LayerNorm, DType::F32, PathKind::VulkanTiled, "layernorm_rows",
         "row-wise LayerNorm with gamma/beta"},
        {"rmsnorm_rows", Op::RmsNorm, DType::F32, PathKind::VulkanTiled, "rmsnorm_rows", "row-wise RMSNorm"},
        {"rope_rows", Op::Rope, DType::F32, PathKind::VulkanScalar, "rope_rows",
         "split-half rotary embedding, one invocation per (row, pair)"},
    };
    return k;
}

const KernelDesc* find_kernel(const std::string& id) {
    for (const auto& k : kernel_catalog())
        if (id == k.id)
            return &k;
    return nullptr;
}

std::string path_key(const KernelDesc& k) {
    return std::string(to_string(k.path)) + ":" + to_string(k.op) + ":" + to_string(k.dtype);
}

namespace {
Gate no(std::string why) {
    return Gate{false, std::move(why)};
}
// Lowest limits the shaders were written for (see the .comp headers).
constexpr std::uint32_t kNeedPush = 16;
}  // namespace

Gate gate_kernel(const KernelDesc& k, const DeviceCaps& c, const Shape* shape) {
    if (!c.compute_queue_family())
        return no("no compute-capable queue family reported");
    if (!can_compute(c.dtype_compute(k.dtype)))
        return no(std::string(to_string(k.dtype)) + " has no compute path on this device (" +
                  to_string(c.dtype_compute(k.dtype)) + ")");

        const bool coop = k.path == PathKind::VulkanCoopMat;
    const std::uint32_t need_invocations = coop ? 64u : 256u;
    if (!c.max_workgroup_invocations)
        return no("max workgroup invocations unknown");
    if (*c.max_workgroup_invocations < need_invocations)
        return no("device allows fewer than " + std::to_string(need_invocations) + " invocations per workgroup");
    if (!c.max_push_constants)
        return no("max push constant size unknown");
    if (*c.max_push_constants < kNeedPush)
        return no("push constants smaller than 16 bytes");

    // Shared memory the shader declares (f32 tile pair 2 KiB, i8 tile pair 2 KiB, row scratch 1 KiB).
    std::uint32_t need_shared = 0;
    if (k.op == Op::Gemm && !coop)
        need_shared = 2048;
    else if (k.op == Op::Softmax || k.op == Op::LayerNorm || k.op == Op::RmsNorm)
        need_shared = 1024;
    if (need_shared > 0) {
        if (!c.max_shared_memory)
            return no("shared memory size unknown");
        if (*c.max_shared_memory < need_shared)
            return no("shared memory below " + std::to_string(need_shared) + " bytes");
    }

    if (k.path == PathKind::VulkanIntDot && !is_yes(c.integer_dot_product))
        return no(std::string("shaderIntegerDotProduct is ") + to_string(c.integer_dot_product));

    if (coop) {
        if (!c.has_coopmat_f16_f32_16x16x16_subgroup())
            return no(std::string("no f16xf16->f32 16x16x16 subgroup-scope cooperative matrix shape reported "
                                  "(cooperative_matrix=") +
                      to_string(c.cooperative_matrix) + ")");
        // One 64-lane workgroup must be exactly one subgroup, otherwise the subgroup-scope matrix is wrong.
        if (!c.subgroup_size || *c.subgroup_size != 64)
            return no("coopmat kernel is written for subgroup size 64 (device reports " +
                      (c.subgroup_size ? std::to_string(*c.subgroup_size) : std::string("unknown")) + ")");
        if (!is_yes(c.shader_float16))
            return no(std::string("shaderFloat16 is ") + to_string(c.shader_float16));
        if (!is_yes(c.storage_16bit))
            return no(std::string("storageBuffer16BitAccess is ") + to_string(c.storage_16bit));
    }

    if (shape != nullptr) {
        if (shape->m == 0 || shape->n == 0 || (k.op == Op::Gemm && shape->k == 0))
            return no("empty shape");
        if (coop && (shape->m % 16 || shape->n % 16 || shape->k % 16))
            return no("coopmat kernel needs m, n, k all multiples of 16");
        if (k.path == PathKind::VulkanIntDot && shape->k % 4 != 0)
            return no("int8 dot kernel needs k % 4 == 0");
        if (k.op == Op::Rope && shape->n % 2 != 0)
            return no("rope needs an even dimension");
    }
    return Gate{true, "all required capabilities reported"};
}

namespace {

// Higher = closer to being usable; used to explain *why* the CPU was chosen.
int rank(const std::string& status) {
    if (status == "promoted")
        return 5;
    if (status == "rejected")
        return 4;
    if (status == "stale")
        return 3;
    if (status == "unverified")
        return 2;
    if (status == "unknown_binary")
        return 1;
    return 0;  // ineligible
}

}  // namespace

Plan plan(Op op, DType dtype, const Shape& shape, const DeviceCaps& caps, const KernelProfile& profile,
          const KernelHashes& hashes, const PlanPolicy& policy) {
    Plan p;
    p.op = op;
    p.dtype = dtype;
    p.shape = shape;
    const std::string op_name = to_string(op), dt_name = to_string(dtype);

    std::string best_status = "none", best_reason;
    int best_rank = -1;
    auto note = [&](const std::string& id, const std::string& status, const std::string& why) {
        if (rank(status) > best_rank) {
            best_rank = rank(status);
            best_status = status;
            best_reason = why;
            if (rank(status) >= 4)
                p.candidate_kernel_id = id;
        }
    };

    bool any = false;
    for (const auto& k : kernel_catalog()) {
        if (k.op != op || k.dtype != dtype)
            continue;
        any = true;
        const Gate g = gate_kernel(k, caps, &shape);
        if (!g.eligible) {
            note(k.id, "ineligible", std::string(k.id) + ": " + g.reason);
            continue;
        }
        const auto h = hashes.find(k.id);
        if (h == hashes.end() || h->second.empty()) {
            note(k.id, "unknown_binary", std::string(k.id) + ": kernel binary hash unknown (HCS Vulkan kernels not built in)");
            continue;
        }
        const std::string bucket = shape_bucket(shape);
        const ProfileEntry* e = profile.find(caps.device_key(), k.id, h->second, bucket);
        if (e == nullptr) {
            if (profile.has_stale(caps.device_key(), k.id, h->second, bucket))
                note(k.id, "stale", std::string(k.id) + ": profile entry is for another driver/shader (stale)");
            else
                note(k.id, "unverified", std::string(k.id) + ": no profile evidence for bucket " + bucket);
            continue;
        }
        const PromotionDecision d = evaluate_promotion(*e, policy.promotion);
        if (d.default_enabled || (d.promoted && policy.allow_unbaselined)) {
            p.kind = k.path;
            p.kernel_id = k.id;
            p.candidate_kernel_id = k.id;
            p.candidate_status = "promoted";
            char b[200];
            std::snprintf(b, sizeof(b), "%s verified on this device: %.2fx vs %s (bucket %s)", k.id, e->speedup(),
                          e->baseline_name.c_str(), bucket.c_str());
            p.reason = b;
            return p;
        }
        if (d.promoted) {
            note(k.id, "promoted", std::string(k.id) + ": promoted but " + d.reasons.back());
        } else {
            note(k.id, "rejected", std::string(k.id) + ": " + (d.reasons.empty() ? "not promoted" : d.reasons.front()));
        }
    }

    p.kind = PathKind::Reference;
    // Only a kernel that has measured evidence (rejected or promoted) is a candidate; stale/unverified/ineligible are
    // explained in `reason` but are not candidates.
    p.candidate_status = best_rank >= 4 ? best_status : "";
    if (!any)
        p.reason = "no HCS Vulkan kernel exists for " + op_name + " " + dt_name + "; CPU reference used";
    else
        p.reason = "no verified " + op_name + " path for " + dt_name + " on this device yet; CPU reference used (" +
                   best_reason + ")";
    return p;
}

}  // namespace hcs::hardware
