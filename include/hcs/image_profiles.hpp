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
#include <optional>
#include <string>
#include <string_view>

namespace hcs {

// One measured quality/time point (steps + text CFG) of a preset.
struct ImagePresetPoint {
    std::uint32_t steps;
    float cfg;
};

struct ImageProfile {
    std::string_view family;      // "sd15" | "sdturbo" | "sdxlturbo" | "flux3"
    std::uint32_t default_width;  // default size when the caller gives none
    std::uint32_t default_height;
    std::uint32_t default_steps;
    float default_cfg;             // text CFG applied when the caller leaves cfg_scale at 0
    std::string_view sampler;      // stable-diffusion.cpp sample.method name ("" = engine default)
    std::uint32_t min_short_side;  // below this the family is outside its quality envelope
    std::uint32_t max_long_side;   // above this too (0 = no cap): distilled checkpoints only work near their size
    std::uint32_t min_steps;
    std::uint32_t max_steps;       // above this too (0 = no cap): Turbo models over-bake beyond a few steps
    ImagePresetPoint fast;         // "fast" preset
    ImagePresetPoint quality;      // "quality" preset
};

// SD1.5 was trained at 512x512; below ~384 px the composition collapses, and below ~8 steps the sampler has
// not denoised. FLUX.3 Klein is step-distilled (measured 6-step "fast" and 16-step "quality" presets, cfg
// 1.0); its envelope allows the measured 4-step point and 256 px short sides only via allow_low_quality.
// SD-Turbo (SD 2.1 distilled, 512 px only) and SDXL-Turbo (512-768 px) are adversarially distilled for 1-4
// steps without CFG: more steps over-bake them, so the envelope caps steps at 4.
// The preset points are measured values, see docs/test-evidence/wave-c/i1/README.md.
inline constexpr ImageProfile kImageProfiles[] = {
    {"sd15", 512, 512, 20, 7.0f, "euler_a", 384, 0, 8, 0, {12, 7.0f}, {20, 7.0f}},
    {"sdturbo", 512, 512, 1, 1.0f, "euler_a", 512, 512, 1, 4, {1, 1.0f}, {4, 1.0f}},
    {"sdxlturbo", 512, 512, 1, 1.0f, "euler_a", 512, 768, 1, 4, {1, 1.0f}, {4, 1.0f}},
    {"flux3", 512, 512, 16, 1.0f, "", 384, 0, 4, 0, {6, 1.0f}, {16, 1.0f}},
};

inline const ImageProfile* image_profile(std::string_view family) {
    for (const auto& p : kImageProfiles)
        if (p.family == family)
            return &p;
    return nullptr;
}

// The measured point of a named preset ("fast" | "quality") for a family; nullopt when the family has no
// profile or the preset name is unknown.
inline std::optional<ImagePresetPoint> image_preset(std::string_view family, std::string_view name) {
    const ImageProfile* p = image_profile(family);
    if (!p)
        return std::nullopt;
    if (name == "fast")
        return p->fast;
    if (name == "quality")
        return p->quality;
    return std::nullopt;
}

// The architecture a family's checkpoints derive from; this is what a LoRA's manifest `base_family` names.
// SD-Turbo is an SD 2.1 derivative and SDXL-Turbo an SDXL derivative, so an SD2/SDXL LoRA is "compatible"
// by architecture (an SD 1.5 LoRA, e.g. LCM-LoRA-SD1.5, is not).
inline std::string image_base_family(std::string_view family) {
    if (family == "sdturbo")
        return "sd2";
    if (family == "sdxlturbo")
        return "sdxl";
    return std::string(family);
}

// Sampler / scheduler names a request may override (stable-diffusion.cpp `sample.method` / `sample.scheduler`).
// An allow-list, not a pass-through: the strings come from API callers and reach the engine's option parser.
inline bool is_known_image_sampler(std::string_view s) {
    static constexpr std::string_view kNames[] = {"euler",   "euler_a",  "heun",          "dpm2",     "dpm++2s_a",
                                                  "dpm++2m", "ipndm",    "lcm",           "ddim_trailing",
                                                  "tcd",     "res_multistep", "lms"};
    for (const auto n : kNames)
        if (n == s)
            return true;
    return false;
}
inline bool is_known_image_scheduler(std::string_view s) {
    static constexpr std::string_view kNames[] = {"discrete", "karras",  "exponential", "ays", "gits",
                                                  "sgm_uniform", "simple", "smoothstep", "lcm", "beta"};
    for (const auto n : kNames)
        if (n == s)
            return true;
    return false;
}
// What can be read from a checkpoint's tensors without loading any weights (see probe_gguf): the shape of
// the text cross-attention key projection is the architecture's fingerprint.
struct ImageModelSignature {
    std::uint32_t cross_attn_dim = 0;  // ne[0] of ...attn2.to_k.weight: 768 SD1.x, 1024 SD2.x, 2048 SDXL; 0 = unseen
    bool has_label_emb = false;        // SDXL's size/crop conditioning embedding
    bool has_flux_blocks = false;      // FLUX double/single stream blocks
};

namespace detail {
inline std::string lower_copy(std::string_view text) {
    std::string low(text);
    std::transform(low.begin(), low.end(), low.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return low;
}
inline bool has_sub(const std::string& low, const char* s) { return low.find(s) != std::string::npos; }
}  // namespace detail

// Best-effort family guess from a model id / file name. Returns "" when nothing matches (then only the
// engine, which sees the loaded model, can decide).
inline std::string guess_image_family(std::string_view text) {
    const std::string low = detail::lower_copy(text);
    auto has = [&](const char* s) { return detail::has_sub(low, s); };
    if (has("flux"))
        return "flux3";
    if (has("turbo"))
        return (has("xl") ? "sdxlturbo" : "sdturbo");
    if (has("sd15") || has("sd1.5") || has("sd-1.5") || has("sd_1.5") || has("v1-5") || has("v1_5") ||
        has("stable-diffusion-v1") || has("stable-diffusion-1"))
        return "sd15";
    return {};
}

// Family from the tensor structure, with the name only deciding what the structure cannot: Turbo models
// have exactly the tensor shapes of SD 2.1 / SDXL, so "turbo" in the name picks the distilled profile.
// A structure that does not match a known architecture falls back to the name guess.
inline std::string classify_image_family(const ImageModelSignature& sig, std::string_view name_hint) {
    const std::string low = detail::lower_copy(name_hint);
    const bool turbo = detail::has_sub(low, "turbo");
    if (sig.has_flux_blocks)
        return "flux3";
    if (sig.cross_attn_dim == 768)
        return "sd15";
    if (sig.cross_attn_dim == 1024)
        return turbo ? "sdturbo" : "sd2";
    if (sig.cross_attn_dim == 2048 || (sig.cross_attn_dim == 0 && sig.has_label_emb))
        return turbo ? "sdxlturbo" : "sdxl";
    return guess_image_family(name_hint);
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
    auto add = [&](const std::string& s) {
        if (!why.empty())
            why += "; ";
        why += s;
    };
    const std::string fam(p->family);
    const std::uint32_t short_side = std::min(width, height);
    const std::uint32_t long_side = std::max(width, height);
    const std::string size = std::to_string(width) + "x" + std::to_string(height);
    if (short_side < p->min_short_side)
        add("size " + size + " is below the " + fam + " minimum short side of " +
            std::to_string(p->min_short_side) + " px");
    if (p->max_long_side != 0 && long_side > p->max_long_side)
        add("size " + size + " is above the " + fam + " maximum long side of " +
            std::to_string(p->max_long_side) + " px");
    if (!steps_from_preset && steps < p->min_steps)
        add(std::to_string(steps) + " steps is below the " + fam + " minimum of " +
            std::to_string(p->min_steps));
    if (!steps_from_preset && p->max_steps != 0 && steps > p->max_steps)
        add(std::to_string(steps) + " steps is above the " + fam + " maximum of " +
            std::to_string(p->max_steps));
    if (why.empty())
        return {};
    return why + " (recommended " + std::to_string(p->default_width) + "x" +
           std::to_string(p->default_height) + ", " + std::to_string(p->default_steps) +
           " steps); results will be unusable. Pass allow_low_quality=true to render anyway";
}
}  // namespace hcs


