// HCSInfer kernel benchmark (AGENTS.md 12.5 / 16.6 B3): the HCS Vulkan Q8_0 GEMV against the CPU
// reference, per shape. It measures the *isolated kernel*, never a full model. Numbers are for the machine
// printed in the JSON; an equal or slower Vulkan kernel is reported as such and is not default-enabled.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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
    const std::vector<Shape> shapes = {{512, 512},   {1024, 1024},  {2048, 2048},
                                       {4096, 4096}, {4096, 11008}, {11008, 4096}};
    nlohmann::json results = nlohmann::json::array();
    bool any_fail = false;
    for (const Shape s : shapes) {
        const auto wf = hcs::test::random_vec(static_cast<std::size_t>(s.m) * s.k, 1);
        const auto w = hcs::test::to_q8_0(wf, s.m, s.k);
        const auto x = hcs::test::random_vec(s.k, 2);
        std::vector<float> y_cpu(s.m, 0.0f), y_vk(s.m, 0.0f);
        hcs::kernels::cpu::gemv_q8_0(x.data(), w.data(), y_cpu.data(), s.m, s.k);
        auto r = hcs::kernels::vulkan::gemv_q8_0({x.data(), w.data(), y_vk.data(), s.m, s.k}, device);
        nlohmann::json e;
        e["m"] = s.m;
        e["k"] = s.k;
        if (!r.ok()) {
            e["result_status"] = "FAIL";
            e["error"] = r.error().message;
            any_fail = true;
            results.push_back(std::move(e));
            continue;
        }
        double max_err = 0.0;
        for (std::uint32_t i = 0; i < s.m; ++i)
            max_err = std::max(max_err, static_cast<double>(std::fabs(y_vk[i] - y_cpu[i])));
        // warm-up
        (void)hcs::kernels::vulkan::gemv_q8_0({x.data(), w.data(), y_vk.data(), s.m, s.k}, device);
        e["cpu_ms_median"] = median_ms(
            [&] { hcs::kernels::cpu::gemv_q8_0(x.data(), w.data(), y_cpu.data(), s.m, s.k); }, reps);
        e["vulkan_ms_median"] = median_ms(
            [&] {
                (void)hcs::kernels::vulkan::gemv_q8_0({x.data(), w.data(), y_vk.data(), s.m, s.k}, device);
            },
            reps);
        e["max_abs_err"] = max_err;
        e["speedup_cpu_over_vulkan"] = e["cpu_ms_median"].get<double>() / e["vulkan_ms_median"].get<double>();
        // Effective weight bandwidth (Q8_0 bytes read once).
        const double bytes = static_cast<double>(hcs::kernels::cpu::q8_0_bytes(s.m, s.k));
        e["vulkan_gib_s"] =
            bytes / (e["vulkan_ms_median"].get<double>() / 1000.0) / (1024.0 * 1024.0 * 1024.0);
        const bool ok = max_err < 1e-2 * (1.0 + std::fabs(*std::max_element(y_cpu.begin(), y_cpu.end())));
        e["result_status"] = ok ? "PASS" : "FAIL";
        any_fail = any_fail || !ok;
        results.push_back(std::move(e));
    }
    out["shapes"] = std::move(results);
    out["result_status"] = any_fail ? "PARTIAL" : "PASS";
    std::cout << out.dump(2) << "\n";
    return any_fail ? 1 : 0;
}
