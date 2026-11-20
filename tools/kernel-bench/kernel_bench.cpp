// HCSInfer kernel benchmark (AGENTS.md 12.5 / 16.6 B3): the HCS Vulkan Q8_0 GEMV against the CPU
// reference, per shape. It measures the *isolated kernel*, never a full model. Numbers are for the machine
// printed in the JSON; an equal or slower Vulkan kernel is reported as such and is not default-enabled.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hcs/kernels/cpu.hpp"
#include "hcs/kernels/vulkan.hpp"
#include "q8_0_data.hpp"

namespace {

using Clock = std::chrono::steady_clock;

template <typename Fn>
double median_ms(Fn&& fn, int reps) {
    std::vector<double> t;
    t.reserve(static_cast<std::size_t>(reps));
    for (int i = 0; i < reps; ++i) {
        const auto a = Clock::now();
        fn();
        t.push_back(std::chrono::duration<double, std::milli>(Clock::now() - a).count());
    }
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
    int reps = 20;
    std::string device;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--reps" && i + 1 < argc)
            reps = std::max(1, std::atoi(argv[++i]));
        else if (a == "--device" && i + 1 < argc)
            device = argv[++i];
    }

    nlohmann::json out;
    out["schema_version"] = 1;
    out["kernel"] = "gemv_q8_0";
    out["reps"] = reps;

    if (!hcs::kernels::vulkan::available()) {
        out["result_status"] = "BLOCKED";
        out["reason"] = "no Vulkan compute device available";
        std::cout << out.dump(2) << "\n";
        return 2;
    }
    auto info = hcs::kernels::vulkan::device_info(device);
    if (!info.ok()) {
        out["result_status"] = "BLOCKED";
        out["reason"] = info.error().message;
        std::cout << out.dump(2) << "\n";
        return 2;
    }
    out["device"] = {{"name", info.value().name},
                     {"type", info.value().device_type},
                     {"vendor_id", info.value().vendor_id},
                     {"device_id", info.value().device_id}};
    hcs::kernels::vulkan::set_enabled(true);

    struct Shape {
        std::uint32_t m, k;
    };
    const std::vector<Shape> shapes = {
        {512, 512},    {1024, 1024},  {2048, 2048}, {4096, 4096},
        {4096, 11008}, {11008, 4096}, {4096, 14336}};  // 4096x14336 matches the upstream test-backend-ops
                                                       // MUL_MAT perf shape
    nlohmann::json results = nlohmann::json::array();
    bool any_fail = false;
    const std::vector<hcs::kernels::vulkan::Variant> variants = {hcs::kernels::vulkan::Variant::Scalar,
                                                                 hcs::kernels::vulkan::Variant::Tiled};
    for (const Shape s : shapes) {
        const auto wf = hcs::test::random_vec(static_cast<std::size_t>(s.m) * s.k, 1);
        const auto w = hcs::test::to_q8_0(wf, s.m, s.k);
        const auto x = hcs::test::random_vec(s.k, 2);
        std::vector<float> y_cpu(s.m, 0.0f), y_vk(s.m, 0.0f);
        hcs::kernels::cpu::gemv_q8_0(x.data(), w.data(), y_cpu.data(), s.m, s.k);
        nlohmann::json e;
        e["m"] = s.m;
        e["k"] = s.k;
        e["cpu_ms_median"] = median_ms(
            [&] { hcs::kernels::cpu::gemv_q8_0(x.data(), w.data(), y_cpu.data(), s.m, s.k); }, reps);
        const double bytes = static_cast<double>(hcs::kernels::cpu::q8_0_bytes(s.m, s.k));
        // Floor of the per-call host cost: gemv_q8_0() currently allocates host-visible buffers and memcpy's
        // the whole weight matrix on every call, so its wall time includes at least one such copy. The
        // upstream GGML Vulkan path keeps weights resident, so the two are only comparable after subtracting
        // this.
        {
            std::vector<std::uint8_t> scratch(static_cast<std::size_t>(bytes));
            e["weight_memcpy_ms_median"] =
                median_ms([&] { std::memcpy(scratch.data(), w.data(), scratch.size()); }, reps);
        }
        bool shape_ok = true;
        for (const auto variant : variants) {
            const std::string name = hcs::kernels::vulkan::to_string(variant);
            auto r =
                hcs::kernels::vulkan::gemv_q8_0({x.data(), w.data(), y_vk.data(), s.m, s.k}, device, variant);
            nlohmann::json v;
            if (!r.ok()) {
                v["result_status"] = "FAIL";
                v["error"] = r.error().message;
                shape_ok = false;
                any_fail = true;
                e[name] = std::move(v);
                continue;
            }
            double max_err = 0.0;
            for (std::uint32_t i = 0; i < s.m; ++i)
                max_err = std::max(max_err, static_cast<double>(std::fabs(y_vk[i] - y_cpu[i])));
            // warm-up
            (void)hcs::kernels::vulkan::gemv_q8_0({x.data(), w.data(), y_vk.data(), s.m, s.k}, device,
                                                  variant);
            const double ms = median_ms(
                [&] {
                    (void)hcs::kernels::vulkan::gemv_q8_0({x.data(), w.data(), y_vk.data(), s.m, s.k}, device,
                                                          variant);
                },
                reps);
            v["ms_median"] = ms;
            v["max_abs_err"] = max_err;
            v["speedup_cpu_over_vulkan"] = e["cpu_ms_median"].get<double>() / ms;
            v["gib_s"] = bytes / (ms / 1000.0) / (1024.0 * 1024.0 * 1024.0);
            const bool ok = max_err < 1e-2 * (1.0 + std::fabs(*std::max_element(y_cpu.begin(), y_cpu.end())));
            v["result_status"] = ok ? "PASS" : "FAIL";
            shape_ok = shape_ok && ok;
            any_fail = any_fail || !ok;
            e[name] = std::move(v);
        }
        e["result_status"] = shape_ok ? "PASS" : "FAIL";
        results.push_back(std::move(e));
    }
    out["shapes"] = std::move(results);
    out["result_status"] = any_fail ? "PARTIAL" : "PASS";
    std::cout << out.dump(2) << "\n";
    return any_fail ? 1 : 0;
}
