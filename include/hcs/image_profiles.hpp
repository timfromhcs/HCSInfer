#pragma once
// Per-family image-generation profiles: defaults and the minimum quality envelope.
//
// Why this exists: a 256x256 / 2-step SD1.5 render is a colour blob, not an image. Such a request proves only
// that the pipeline runs, so it must be an explicit opt-in (allow_low_quality), never a silent default. The
// envelope is shared by the engine adapter (authoritative: it knows the detected family) and by the control
// plane (early, synchronous rejection when the family can be guessed from the model id/path).
//
// Header-only and dependency-free so it is unit-testable without an engine.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace hcs {

struct ImageProfile {
    std::string_view family;      // "sd15" | "flux3"
    std::uint32_t default_width;  // default size when the caller gives none
    std::uint32_t default_height;
    std::uint32_t default_steps;
    float default_cfg;             // text CFG applied when the caller leaves cfg_scale at 0
    std::string_view sampler;      // stable-diffusion.cpp sample.method name ("" = engine default)
    std::uint32_t min_short_side;  // below this the family is outside its quality envelope
    std::uint32_t min_steps;
};

// SD1.5 was trained at 512x512; below ~384 px the composition collapses, and below ~8 steps the sampler has
// not denoised. FLUX.3 Klein is step-distilled (measured 6-step "fast" and 16-step "quality" presets, cfg
// 1.0); its envelope allows the measured 4-step point and 256 px short sides only via allow_low_quality.
inline constexpr ImageProfile kImageProfiles[] = {
    {"sd15", 512, 512, 20, 7.0f, "euler_a", 384, 8},
    {"flux3", 512, 512, 16, 1.0f, "", 384, 4},
};

inline const ImageProfile* image_profile(std::string_view family) {
    for (const auto& p : kImageProfiles)
        if (p.family == family)
            return &p;
    return nullptr;
}

// Best-effort family guess from a model id / file name. Returns "" when nothing matches (then only the
// engine, which sees the loaded model, can decide).
inline std::string guess_image_family(std::string_view text) {
    std::string low(text);
    std::transform(low.begin(), low.end(), low.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto has = [&](const char* s) { return low.find(s) != std::string::npos; };
    if (has("flux"))
        return "flux3";
    if (has("sd15") || has("sd1.5") || has("sd-1.5") || has("sd_1.5") || has("v1-5") || has("v1_5") ||
        has("stable-diffusion-v1") || has("stable-diffusion-1"))
        return "sd15";
    return {};
}

// Empty string = inside the envelope (or the family has no profile). Otherwise a human-readable reason that
// names the remedy. `steps_from_preset` skips the step check (a preset replaces steps with a measured point).
inline std::string image_envelope_violation(std::string_view family, std::uint32_t width,
                                            std::uint32_t height, std::uint32_t steps,
                                            bool steps_from_preset) {
    const ImageProfile* p = image_profile(family);
    if (!p)
        return {};
    std::string why;
    const std::uint32_t short_side = std::min(width, height);
    if (short_side < p->min_short_side)
        why += "size " + std::to_string(width) + "x" + std::to_string(height) + " is below the " +
               std::string(p->family) + " minimum short side of " + std::to_string(p->min_short_side) + " px";
    if (!steps_from_preset && steps < p->min_steps) {
        if (!why.empty())
            why += "; ";
        why += std::to_string(steps) + " steps is below the " + std::string(p->family) + " minimum of " +
               std::to_string(p->min_steps);
    }
    if (why.empty())
        return {};
    return why + " (recommended " + std::to_string(p->default_width) + "x" +
           std::to_string(p->default_height) + ", " + std::to_string(p->default_steps) +
           " steps); results will be unusable. Pass allow_low_quality=true to render anyway";
}

}  // namespace hcs
