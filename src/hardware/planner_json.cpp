// `doctor --json` planner section: capability gates + profile evidence + the plan table. "unknown = CPU".
#include <filesystem>
#include <system_error>

#include "caps_json.hpp"
#include "hcs/hardware/planner.hpp"

namespace hcs::hardware {

namespace {

json entry_summary(const ProfileEntry& e, const PromotionPolicy& pol) {
    const PromotionDecision d = evaluate_promotion(e, pol);
    json j;
    j["bucket"] = e.shape_bucket;
    j["shape"] = {{"m", e.m}, {"n", e.n}, {"k", e.k}};
    j["correct"] = e.correct;
    j["max_abs_error"] = e.max_abs_error;
    j["identical_runs"] = e.identical_runs;
    j["total_runs"] = e.total_runs;
    j["wall_ms_median"] = e.wall_ms_median;
    j["wall_spread"] = e.wall_spread;
    j["gpu_ms_median"] = e.gpu_ms_median ? json(*e.gpu_ms_median) : unknown();
    j["gflops_wall"] = e.gflops_wall() ? json(*e.gflops_wall()) : unknown();
    j["gflops_gpu"] = e.gflops_gpu() ? json(*e.gflops_gpu()) : unknown();
    j["baseline"] = {{"name", e.baseline_name}, {"ms", e.baseline_ms}};
    j["speedup_vs_baseline"] = e.speedup();
    j["upstream_ggml_vulkan"] = e.upstream_status == "measured" && e.upstream_ratio
                                    ? json{{"status", "measured"}, {"ratio", *e.upstream_ratio}}
                                    : json{{"status", "NOT RUN"}, {"ratio", unknown()}};
    j["promoted"] = d.promoted;
    j["default_enabled"] = d.default_enabled;
    j["reasons"] = d.reasons;
    j["timestamp_utc"] = e.timestamp_utc;
    return j;
}

json plan_json(const Plan& p) {
    json j;
    j["op"] = to_string(p.op);
    j["dtype"] = to_string(p.dtype);
    j["shape"] = {{"m", p.shape.m}, {"n", p.shape.n}, {"k", p.shape.k}};
    j["bucket"] = shape_bucket(p.shape);
    j["path"] = to_string(p.kind);
    j["kernel"] = p.kernel_id.empty() ? json(nullptr) : json(p.kernel_id);
    j["reason"] = p.reason;
    j["candidate_kernel"] = p.candidate_kernel_id.empty() ? json(nullptr) : json(p.candidate_kernel_id);
    j["candidate_status"] = p.candidate_status;
    return j;
}

struct Rep {
    Op op;
    DType dtype;
    Shape shape;
};

}  // namespace

json planner_report(const std::vector<DeviceCaps>& devices, const KernelHashes* hashes,
                    const std::filesystem::path& state_dir) {
    json r;
    const PlanPolicy policy;  // release defaults
    r["rule"] = "unknown = CPU; a kernel is promoted only after correctness vs the CPU oracle AND 3 identical "
                "runs AND >= 1.05x vs the baseline; default-enabled additionally needs a MEASURED upstream "
                "GGML Vulkan baseline >= 1.05x";
    r["policy"] = {{"min_identical_runs", policy.promotion.min_identical_runs},
                   {"min_speedup", policy.promotion.min_speedup},
                   {"max_spread", policy.promotion.max_spread},
                   {"allow_unbaselined", policy.allow_unbaselined}};
    r["kernel_hashes"] = hashes ? "known" : "unknown (HCS Vulkan kernels are not built into this binary)";

    KernelProfile profile;
    json prof;
    if (state_dir.empty()) {
        prof = {{"path", unknown()}, {"status", "unknown (no state dir)"}, {"entries", 0}};
    } else {
        const auto path = profile_path(state_dir);
        std::error_code ec;
        const bool exists = std::filesystem::exists(path, ec);
        auto loaded = load_profile(path);
        if (loaded) {
            profile = std::move(loaded.value());
            prof = {{"path", path.string()},
                    {"status", exists ? "ok" : "missing (run `HCSInfer tune`)"},
                    {"entries", profile.entries().size()}};
        } else {
            prof = {{"path", path.string()}, {"status", "error: " + loaded.error().message}, {"entries", 0}};
        }
    }
    r["profile"] = prof;

    static const std::vector<Rep> reps = {
        {Op::Gemm, DType::F32, {256, 1024, 256}},    {Op::Gemm, DType::F16, {256, 1024, 256}},
        {Op::Gemm, DType::I8, {256, 1024, 256}},     {Op::Softmax, DType::F32, {32, 4096, 0}},
        {Op::LayerNorm, DType::F32, {32, 4096, 0}},  {Op::RmsNorm, DType::F32, {32, 4096, 0}},
        {Op::Rope, DType::F32, {512, 128, 0}},
    };
    const KernelHashes none;
    json devs = json::array();
    for (const auto& caps : devices) {
        json d;
        d["name"] = caps.name;
        d["device_key"] = caps.device_key();
        d["caps"] = caps_to_json(caps);
        json ks = json::array();
        for (const auto& k : kernel_catalog()) {
            json kj;
            kj["id"] = k.id;
            kj["op"] = to_string(k.op);
            kj["dtype"] = to_string(k.dtype);
            kj["path"] = to_string(k.path);
            kj["evidence_key"] = path_key(k);
            const Gate g = gate_kernel(k, caps);
            kj["gate"] = {{"eligible", g.eligible}, {"reason", g.reason}};
            if (!g.eligible)
                kj["status"] = "NOT RUN on this hardware";
            json ev = json::array();
            std::string hash = "unknown";
            if (hashes) {
                if (const auto it = hashes->find(k.id); it != hashes->end())
                    hash = it->second;
            }
            kj["kernel_hash"] = hash;
            for (const auto& e : profile.entries())
                if (e.device_key == caps.device_key() && e.kernel_id == k.id)
                    ev.push_back(entry_summary(e, policy.promotion));
            kj["evidence"] = ev;
            if (g.eligible)
                kj["status"] = ev.empty() ? "unverified" : "measured";
            ks.push_back(std::move(kj));
        }
        d["kernels"] = ks;
        json plans = json::array();
        for (const auto& rep : reps)
            plans.push_back(plan_json(plan(rep.op, rep.dtype, rep.shape, caps, profile, hashes ? *hashes : none, policy)));
        d["plans"] = plans;
        devs.push_back(std::move(d));
    }
    r["devices"] = devs;
    return r;
}

}  // namespace hcs::hardware
