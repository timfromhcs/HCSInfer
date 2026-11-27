// hcs_ggml_vk_bench: the pinned upstream GGML Vulkan backend (what llama.cpp / stable-diffusion.cpp really
// run), driven in-process with one-node graphs for the operations the HCS Vulkan kernels cover plus quantised
// matmuls, on real LLM and SD-like shapes. JSON per AGENTS.md 21.1 (schema_version 1). It measures operators,
// never a model; model-level numbers (tokens/s) are produced by the real-model runs under
// docs/test-evidence/.
//
//   hcs_ggml_vk_bench [--device <name substring>] [--suite quick|full|llm] [--samples N] [--out file.json]
//                     [--no-quant]
//
// Honesty: every upstream result is spot-checked against a double-precision reference before it is timed; a
// shape the backend cannot run is reported with the reason, never skipped silently.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "hcs/hardware/caps.hpp"
#include "hcs/hardware/doctor.hpp"
#include "hcs/kernels/ggml_vk_baseline.hpp"
#include "hcs/version.hpp"

namespace gb = hcs::kernels::ggml_baseline;
using json = nlohmann::json;

namespace {

struct Case {
    enum class Op { MulMat, Softmax, LayerNorm, RmsNorm, Rope } op;
    gb::WeightType w = gb::WeightType::F32;
    std::uint32_t m = 0, n = 0, k = 0;
    const char* tag = "";  // where the shape comes from
};

std::vector<Case> cases_for(const std::string& suite, bool quant) {
    using Op = Case::Op;
    using W = gb::WeightType;
    std::vector<Case> c;
    auto gemm = [&](std::uint32_t m, std::uint32_t n, std::uint32_t k, const char* tag,
                    bool also_quant = false) {
        c.push_back({Op::MulMat, W::F32, m, n, k, tag});
        c.push_back({Op::MulMat, W::F16, m, n, k, tag});
        if (also_quant && quant) {
            c.push_back({Op::MulMat, W::Q8_0, m, n, k, tag});
            c.push_back({Op::MulMat, W::Q4_K, m, n, k, tag});
        }
    };
    // Same shapes as `HCSInfer tune --suite quick|full`.
    gemm(256, 1024, 256, "tune quick");
    c.push_back({Op::Softmax, W::F32, 32, 4096, 0, "tune quick"});
    c.push_back({Op::LayerNorm, W::F32, 32, 4096, 0, "tune quick"});
    c.push_back({Op::RmsNorm, W::F32, 32, 4096, 0, "tune quick"});
    c.push_back({Op::Rope, W::F32, 512, 128, 0, "tune quick"});
    if (suite == "quick")
        return c;
    gemm(128, 128, 128, "tune full");
    gemm(512, 2048, 512, "tune full");
    gemm(1024, 1024, 1024, "tune full");
    c.push_back({Op::Softmax, W::F32, 256, 1024, 0, "tune full"});
    c.push_back({Op::Softmax, W::F32, 4, 32000, 0, "tune full"});
    c.push_back({Op::LayerNorm, W::F32, 256, 1024, 0, "tune full"});
    c.push_back({Op::LayerNorm, W::F32, 4, 32000, 0, "tune full"});
    c.push_back({Op::RmsNorm, W::F32, 256, 1024, 0, "tune full"});
    c.push_back({Op::RmsNorm, W::F32, 4, 32000, 0, "tune full"});
    c.push_back({Op::Rope, W::F32, 2048, 128, 0, "tune full"});
    if (suite == "full")
        return c;
    // Real shapes (m = tokens, n = output features, k = input features).
    gemm(1, 1024, 3072, "decode GEMV, qwen3-0.6b FFN up", true);
    gemm(1, 4096, 4096, "decode GEMV, 7-8B attention projection", true);
    gemm(1, 14336, 4096, "decode GEMV, Llama-8B FFN up", true);
    gemm(1, 4096, 14336, "decode GEMV, Llama-8B FFN down", true);
    gemm(128, 4096, 4096, "prefill 128 tokens", true);
    gemm(512, 4096, 4096, "prefill 512 tokens", true);
    gemm(1024, 1024, 64, "SD attention QK^T (one head, 32x32 latent)");
    gemm(4096, 320, 320, "SD 1x1 conv / attention projection at 64x64 latent");
    gemm(1024, 320, 2880, "SD 3x3 conv as GEMM at 32x32 latent, 320 channels");
    c.push_back({Op::Softmax, W::F32, 1024, 1024, 0, "SD attention scores, one head"});
    c.push_back({Op::Softmax, W::F32, 64, 14336, 0, "prefill logits-like"});
    c.push_back({Op::RmsNorm, W::F32, 512, 4096, 0, "prefill 512 tokens"});
    c.push_back({Op::LayerNorm, W::F32, 4096, 320, 0, "SD group/layer norm rows"});
    c.push_back({Op::Rope, W::F32, 1, 128, 0, "decode, one token"});
    c.push_back({Op::Rope, W::F32, 4096, 128, 0, "prefill 4096 tokens"});
    return c;
}

std::string utc_now() {
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char b[32];
    std::strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return b;
}

const char* op_name(Case::Op o) {
    switch (o) {
        case Case::Op::MulMat:
            return "mul_mat";
        case Case::Op::Softmax:
            return "softmax";
        case Case::Op::LayerNorm:
            return "layernorm";
        case Case::Op::RmsNorm:
            return "rmsnorm";
        case Case::Op::Rope:
            return "rope";
    }
    return "?";
}

}  // namespace

int main(int argc, char** argv) {
    std::string device, suite = "llm", out_file;
    std::uint32_t samples = 15;
    bool quant = true;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](std::string& dst) {
            if (i + 1 >= argc) {
                std::cerr << a << " needs a value\n";
                std::exit(2);
            }
            dst = argv[++i];
        };
        if (a == "--device")
            need(device);
        else if (a == "--suite")
            need(suite);
        else if (a == "--out")
            need(out_file);
        else if (a == "--samples") {
            std::string v;
            need(v);
            samples = static_cast<std::uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
        } else if (a == "--no-quant")
            quant = false;
        else if (a == "--help" || a == "-h") {
            std::cout
                << "hcs_ggml_vk_bench [--device <name substring>] [--suite quick|full|llm] [--samples N>=10]"
                   " [--out file.json] [--no-quant]\n";
            return 0;
        } else {
            std::cerr << "unexpected argument: " << a << "\n";
            return 2;
        }
    }
    if (suite != "quick" && suite != "full" && suite != "llm") {
        std::cerr << "--suite must be quick, full or llm\n";
        return 2;
    }
    if (samples < 10 || samples > 1000) {
        std::cerr << "--samples must be in 10..1000 (>= 10 timed repetitions are required)\n";
        return 2;
    }

    auto session = gb::Session::create(device);
    if (!session) {
        std::cerr << "error: " << session.error().message << "\n";
        return session.error().exit_code();
    }

    // Device/driver metadata from HCS's own Vulkan probe, matched by name to the ggml device.
    json dev = json::object();
    {
        std::string st, rs;
        const auto caps = hcs::hardware::probe_vulkan_caps(&st, &rs);
        const std::string want = session.value().device().description;
        const hcs::hardware::DeviceCaps* hit = nullptr;
        for (const auto& c : caps)
            if (c.name == want || want.find(c.name) != std::string::npos ||
                c.name.find(want) != std::string::npos)
                hit = &c;
        if (!hit && !caps.empty())
            hit = &caps.front();
        if (hit) {
            dev = {{"name", hit->name},
                   {"vendor_id", hit->vendor_id},
                   {"device_id", hit->device_id},
                   {"api_version", hit->api_version},
                   {"driver_version_raw", hit->driver_version_raw},
                   {"driver_name", hit->driver_name.empty() ? "unknown" : hit->driver_name},
                   {"driver_info", hit->driver_info.empty() ? "unknown" : hit->driver_info},
                   {"subgroup_size", hit->subgroup_size ? json(*hit->subgroup_size) : json("unknown")}};
        } else {
            dev = {{"name", want}, {"note", "HCS Vulkan probe returned no device: driver metadata unknown"}};
        }
    }

    json env = json::object();
    {
        hcs::hardware::DoctorOptions dopt;
        bool ok = false;
        const std::string text = hcs::hardware::collect_report_json(dopt, &ok);
        env = json::parse(text, nullptr, false);
        if (env.is_discarded())
            env = json::object({{"note", "doctor report unavailable"}});
    }

    json results = json::array();
    int errors = 0;
    std::cout << "ggml Vulkan operator baseline on '" << session.value().device().description << "' (suite "
              << suite << ", " << samples << " samples)\n";
    std::printf("%-10s %-5s %-22s %10s %10s %8s %10s  %s\n", "op", "w", "shape (m,n,k)", "median ms",
                "p90 ms", "spread", "GFLOP/s", "variant / note");
    for (const Case& c : cases_for(suite, quant)) {
        hcs::Result<gb::Measurement> r = hcs::Error(hcs::ErrorCode::Internal, "unset");
        switch (c.op) {
            case Case::Op::MulMat:
                r = session.value().mul_mat(c.w, c.m, c.n, c.k, samples);
                break;
            case Case::Op::Softmax:
                r = session.value().softmax(c.m, c.n, samples);
                break;
            case Case::Op::LayerNorm:
                r = session.value().layernorm(c.m, c.n, 1e-5f, samples);
                break;
            case Case::Op::RmsNorm:
                r = session.value().rmsnorm(c.m, c.n, 1e-5f, samples);
                break;
            case Case::Op::Rope:
                r = session.value().rope(c.m, c.n, 0, samples);
                break;
        }
        json j;
        j["op"] = op_name(c.op);
        j["weight_type"] = c.op == Case::Op::MulMat ? std::string(gb::to_string(c.w)) : "f32";
        j["shape"] = {{"m", c.m}, {"n", c.n}, {"k", c.k}};
        j["shape_source"] = c.tag;
        if (r) {
            const gb::Measurement& mm = r.value();
            j["status"] = "PASS";
            j["variant"] = mm.variant;
            j["note"] = mm.note;
            j["samples"] = mm.samples;
            j["warmup"] = "10 runs minimum, until 250 ms elapsed (max 400)";
            j["median_ms"] = mm.median_ms;
            j["p10_ms"] = mm.p10_ms;
            j["p90_ms"] = mm.p90_ms;
            j["spread_p10_p90_over_median"] = mm.spread;
            j["gflops_median"] = mm.flops > 0.0 && mm.median_ms > 0.0
                                     ? json(mm.flops / (mm.median_ms * 1e-3) / 1e9)
                                     : json("unknown");
            j["spot_check_max_err_over_l1"] = mm.max_err_over_l1;
            j["raw_ms"] = mm.raw_ms;
            char shp[48];
            std::snprintf(shp, sizeof(shp), "%u,%u,%u", c.m, c.n, c.k);
            std::printf("%-10s %-5s %-22s %10.4f %10.4f %8.3f %10.1f  %s\n", op_name(c.op),
                        c.op == Case::Op::MulMat ? std::string(gb::to_string(c.w)).c_str() : "f32", shp,
                        mm.median_ms, mm.p90_ms, mm.spread,
                        mm.flops > 0.0 && mm.median_ms > 0.0 ? mm.flops / (mm.median_ms * 1e-3) / 1e9 : 0.0,
                        mm.variant.c_str());
        } else {
            j["status"] = r.error().code == hcs::ErrorCode::Unsupported
                              ? "NOT RUN (unsupported by the backend)"
                              : "ERROR";
            j["reason"] = r.error().message;
            if (r.error().code != hcs::ErrorCode::Unsupported)
                ++errors;
            std::printf("%-10s %-5s %-22s -- %s: %s\n", op_name(c.op),
                        c.op == Case::Op::MulMat ? std::string(gb::to_string(c.w)).c_str() : "f32",
                        (std::to_string(c.m) + "," + std::to_string(c.n) + "," + std::to_string(c.k)).c_str(),
                        j["status"].get<std::string>().c_str(), r.error().message.c_str());
        }
        results.push_back(std::move(j));
    }

    json doc;
    doc["schema_version"] = 1;
    doc["generator"] = "hcs_ggml_vk_bench";
    doc["timestamp_utc"] = utc_now();
    doc["git_commit"] = HCS_GIT_COMMIT;
    doc["build_type"] = HCS_BUILD_TYPE;
    doc["backend"] = "Vulkan (upstream ggml, pinned external/llama.cpp)";
    doc["model"] = "none: synthetic one-node operator graphs";
    doc["model_sha256"] = "n/a";
    doc["storage_source"] = "n/a (random host data uploaded once)";
    doc["cold_or_warm"] = "warm (inputs resident, pipelines created during warm-up)";
    doc["suite"] = suite;
    doc["measured_iterations"] = samples;
    doc["warmup_iterations"] = "10 minimum, until 250 ms elapsed";
    doc["ggml_device"] = {{"index", session.value().device().ggml_index},
                          {"description", session.value().device().description},
                          {"backend_name", session.value().device().backend},
                          {"memory_free_bytes", session.value().device().memory_free},
                          {"memory_total_bytes", session.value().device().memory_total}};
    doc["device"] = dev;
    doc["environment"] = env;
    doc["method"] =
        "ggml_backend_graph_compute (submit + wait) of a one-node graph per operation; inputs uploaded once "
        "and "
        "device resident; every result spot-checked against a double-precision reference before timing";
    doc["results"] = results;
    doc["result_status"] = errors == 0 ? "PASS" : "FAIL";
    if (!out_file.empty()) {
        std::ofstream f(out_file, std::ios::binary | std::ios::trunc);
        if (!f) {
            std::cerr << "cannot write " << out_file << "\n";
            return 8;
        }
        f << doc.dump(2) << "\n";
    }
    return errors == 0 ? 0 : 4;
}
