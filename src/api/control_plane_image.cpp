// ControlPlane: image generation / editing (text-to-image and img2img through IImageStreamEngine). Kept in
// its own translation unit like control_plane_vision.cpp. Everything an HTTP/MCP caller can influence is
// validated here BEFORE the engine is touched: dimensions, pixel budget, steps, strength, prompt size and the
// init image header (format + decoded pixel count are probed without decoding, AGENTS.md 19.3).
#include <algorithm>
#include <cstdio>

#include "hcs/api/control_plane.hpp"
#include "hcs/api/image_probe.hpp"
#include "hcs/image_profiles.hpp"

namespace hcs::api {

Result<void> ControlPlane::validate_image(const ImageParams& p, bool require_init) const {
    if (p.prompt.empty())
        return make_error(ErrorCode::InvalidArgument, "prompt must not be empty");
    if (p.prompt.size() > opt_.max_prompt_bytes || p.negative_prompt.size() > opt_.max_prompt_bytes)
        return make_error(ErrorCode::InvalidArgument, "prompt exceeds the configured size limit");
    if (!p.model.empty()) {
        if (!is_valid_model_id(p.model))
            return make_error(ErrorCode::InvalidArgument, "invalid model id");
        auto m = registry_.get(p.model);
        if (!m)
            return m.error();
        if (m.value().kind != ModelKind::Image)
            return make_error(ErrorCode::InvalidArgument, "model '" + p.model +
                                                              "' is not an image-generation model (kind is " +
                                                              std::string(to_string(m.value().kind)) + ")");
    }
    constexpr std::uint32_t kMin = 64;
    const std::uint32_t max_dim = opt_.max_generate_dimension;
    if (p.width < kMin || p.height < kMin || p.width > max_dim || p.height > max_dim)
        return make_error(ErrorCode::InvalidArgument,
                          "width/height must be in " + std::to_string(kMin) + ".." + std::to_string(max_dim));
    if (p.width % 8 != 0 || p.height % 8 != 0)
        return make_error(ErrorCode::InvalidArgument, "width/height must be multiples of 8");
    if (static_cast<std::uint64_t>(p.width) * p.height > opt_.max_generate_pixels)
        return make_error(
            ErrorCode::InvalidArgument,
            "requested image has " + std::to_string(static_cast<std::uint64_t>(p.width) * p.height) +
                " pixels, above the configured limit of " + std::to_string(opt_.max_generate_pixels));
    if (p.steps == 0 || p.steps > opt_.max_image_steps)
        return make_error(ErrorCode::InvalidArgument,
                          "steps must be in 1.." + std::to_string(opt_.max_image_steps));
    if (!p.allow_low_quality) {
        // Early, synchronous quality-envelope check when the family can be read from the model id (the engine
        // repeats it authoritatively with the detected family).
        std::string family = guess_image_family(p.model);
        if (family.empty() && p.model.empty()) {
            if (const auto cur = loaded_model())
                family = guess_image_family(*cur);
        }
        if (const auto why = image_envelope_violation(family, p.width, p.height, p.steps, !p.preset.empty());
            !why.empty())
            return make_error(ErrorCode::InvalidArgument, "low_quality_request: " + why);
    }
    if (!(p.strength >= 0.0f && p.strength <= 1.0f))
        return make_error(ErrorCode::InvalidArgument, "strength must be in 0..1");
    if (!(p.cfg_scale >= 0.0f && p.cfg_scale <= 30.0f))
        return make_error(ErrorCode::InvalidArgument, "cfg_scale must be in 0..30");
    if (require_init && !p.init_image)
        return make_error(ErrorCode::InvalidArgument, "an input image is required");
    if (auto u = check_upscale_request(p); !u)
        return u;
    if (!p.loras.empty()) {
        auto l = resolve_loras(p);
        if (!l)
            return l.error();
    }
    if (p.init_image) {
        const auto& img = *p.init_image;
        if (img.bytes.empty())
            return make_error(ErrorCode::InvalidArgument, "input image is empty");
        if (img.bytes.size() > opt_.max_image_bytes)
            return make_error(
                ErrorCode::InvalidArgument,
                "input image exceeds the " + std::to_string(opt_.max_image_bytes) + " byte limit");
        const auto probe = probe_image(img.bytes.data(), img.bytes.size());
        if (!probe || (probe->format != "png" && probe->format != "jpeg"))
            return make_error(ErrorCode::InvalidArgument,
                              "the input image must be a png or jpeg (or its header is truncated)");
        if (probe->pixels() > opt_.max_image_pixels)
            return make_error(ErrorCode::InvalidArgument,
                              "input image dimensions " + std::to_string(probe->width) + "x" +
                                  std::to_string(probe->height) + " exceed the " +
                                  std::to_string(opt_.max_image_pixels) + " pixel limit");
    }
    return {};
}

// --- LoRA library / upscaler
// ------------------------------------------------------------------------------------

Result<void> ControlPlane::scan_loras(const std::filesystem::path& dir, bool verify_hashes) {
    auto fresh = std::make_shared<LoraLibrary>();
    if (auto r = fresh->scan(guard_, dir, LoraScanOptions{verify_hashes}); !r)
        return r;
    std::lock_guard lk(lora_mutex_);
    loras_ = std::move(fresh);
    return {};
}

bool ControlPlane::has_lora_library() const {
    std::lock_guard lk(lora_mutex_);
    return loras_ != nullptr;
}

nlohmann::json ControlPlane::loras_json() const {
    std::shared_ptr<LoraLibrary> lib;
    {
        std::lock_guard lk(lora_mutex_);
        lib = loras_;
    }
    nlohmann::json out = {{"loras", nlohmann::json::array()},
                          {"max_per_request", kMaxLorasPerRequest},
                          {"max_weight", kMaxLoraWeight}};
    if (!lib) {
        out["note"] = "no LoRA library: create <approved root>/lora/image/manifest.json (see docs/lora.md)";
        return out;
    }
    out["note"] = lib->scan_note();
    for (const auto& e : lib->entries()) {
        out["loras"].push_back({{"id", e.id},
                                {"name", e.name},
                                {"base_family", e.base_family},
                                {"trigger_words", e.trigger_words},
                                {"recommended_weight", e.recommended_weight},
                                {"licence", e.licence},
                                {"source", e.source},
                                {"sha256", e.sha256},
                                {"sha256_status", e.sha256_status},
                                {"size_bytes", e.size_bytes},
                                {"status", e.status},
                                {"usable", e.usable()},
                                {"problems", e.problems}});
    }
    return out;
}

void ControlPlane::set_upscaler(std::shared_ptr<upscale::IUpscaleEngine> engine) {
    upscaler_ = std::move(engine);
}

Result<std::vector<LoraSpec>> ControlPlane::resolve_loras(const ImageParams& p) const {
    if (p.loras.empty())
        return std::vector<LoraSpec>{};
    std::shared_ptr<LoraLibrary> lib;
    {
        std::lock_guard lk(lora_mutex_);
        lib = loras_;
    }
    if (!lib)
        return make_error(
            ErrorCode::Unsupported,
            "no LoRA library is available: create <approved model root>/lora/image/manifest.json "
            "(see docs/lora.md)");
    // Family of the model the LoRAs will be applied to: from its id or its file name when recognisable. The
    // engine repeats the check with the family it detects from the loaded weights (authoritative).
    const std::string target = !p.model.empty() ? p.model : loaded_model().value_or(std::string());
    std::string family = guess_image_family(target);
    if (family.empty() && !target.empty())
        if (auto m = registry_.get(target); m)
            family = guess_image_family(m.value().path);
    auto specs = lib->resolve(p.loras, family, /*allow_unknown_family=*/true);
    if (!specs)
        return specs.error();
    for (auto& s : specs.value()) {  // the approved-root check is repeated at use time, not only at scan time
        auto g = guard_.resolve(s.path);
        if (!g)
            return g.error();
        s.path = g.value().string();
    }
    return specs;
}

Result<void> ControlPlane::check_upscale_request(const ImageParams& p) const {
    if (p.upscale.empty() || p.upscale == "off")
        return {};
    if (p.upscale != "4k")
        return make_error(ErrorCode::InvalidArgument, "upscale must be \"off\" or \"4k\"");
    if (!upscaler_)
        return make_error(ErrorCode::Unsupported, "no upscaler is configured in this build or process");
    // The runtime and the model must exist BEFORE the (possibly very long) render starts.
    if (auto r = upscaler_->ready(); !r)
        return make_error(r.error().code, "upscale \"4k\" is not available: " + r.error().message);
    return {};
}

Result<upscale::Plan> ControlPlane::plan_upscale_request(const UpscaleParams& p) const {
    if (!upscaler_)
        return make_error(ErrorCode::Unsupported, "no upscaler is configured in this build or process");
    const auto& img = p.image;
    if (img.bytes.empty())
        return make_error(ErrorCode::InvalidArgument, "input image is empty");
    if (img.bytes.size() > opt_.max_image_bytes)
        return make_error(ErrorCode::InvalidArgument,
                          "input image exceeds the " + std::to_string(opt_.max_image_bytes) + " byte limit");
    const auto probe = probe_image(img.bytes.data(), img.bytes.size());
    if (!probe || probe->format != "png")
        return make_error(ErrorCode::InvalidArgument,
                          "the image to upscale must be a PNG (or its header is truncated)");
    const std::uint64_t max_px = std::min<std::uint64_t>(opt_.max_image_pixels, upscale::kMaxSourcePixels);
    if (probe->pixels() > max_px)
        return make_error(ErrorCode::InvalidArgument, "input image dimensions " +
                                                          std::to_string(probe->width) + "x" +
                                                          std::to_string(probe->height) + " exceed the " +
                                                          std::to_string(max_px) + " pixel limit");
    if (auto r = upscaler_->ready(); !r)
        return make_error(r.error().code, "upscale is not available: " + r.error().message);
    refresh_budget();
    upscale::Options uo;
    uo.memory_budget_bytes = budget_.remaining();
    // The planner refuses (OutOfMemory) when even the smallest tile cannot fit the live budget.
    return upscale::plan_upscale(probe->width, probe->height, upscaler_->info(), uo);
}

Result<void> ControlPlane::validate_upscale(const UpscaleParams& p) const {
    auto plan = plan_upscale_request(p);
    if (!plan)
        return plan.error();
    return {};
}

Result<Submitted> ControlPlane::submit_upscale(const UpscaleParams& p) {
    auto plan = plan_upscale_request(p);
    if (!plan)
        return plan.error();
    auto ticket = admit_job(scheduler::WorkKind::Image, "image.upscale", plan.value().peak_bytes);
    if (!ticket)
        return ticket.error();
    return submit_with_channel(
        "image.upscale", [this, p, tk = ticket.value()](JobContext& ctx) -> Result<void> {
            (void)tk;  // holds the admission slot until the job closure is destroyed
            if (ctx.cancelled())
                return make_error(ErrorCode::Cancelled, "cancelled before start");
            upscale::Options uo;
            refresh_budget();  // re-measured: the machine may have changed since the request was admitted
            uo.memory_budget_bytes = budget_.remaining();
            upscale::Stats st;
            ctx.progress(0.0, "upscaling");
            auto up = upscale::upscale_png(
                *upscaler_, p.image.bytes, uo, ctx.token(),
                [&ctx](double f) { ctx.progress(f, "upscaling"); }, &st);
            if (!up) {
                if (up.error().code == ErrorCode::Cancelled || ctx.cancelled())
                    return make_error(ErrorCode::Cancelled, "image upscaling cancelled");
                return up.error();
            }
            ImageResult res;
            res.format = "png";
            res.bytes = std::move(up.value());
            res.width = st.out_w;
            res.height = st.out_h;
            res.pre_upscale_width = st.src_w;
            res.pre_upscale_height = st.src_h;
            res.upscale_provider = st.provider;
            res.upscale_ms = st.model_ms + st.resample_ms;
            res.generation_ms = res.upscale_ms;
            res.upscale_tiles = st.tiles;
            res.upscale_tile_px = st.tile;
            res.upscale_status = st.skipped ? "skipped" : "done";
            if (st.skipped)
                res.upscale_detail = st.skip_reason;
            note_image_result(ctx.id(), res);
            return {};
        });
}

void ControlPlane::note_image_result(JobId id, const ImageResult& r) {
    std::lock_guard lk(results_mutex_);
    image_results_[id.value] = r;
    // Encoded images are large: keep only the most recent few.
    while (image_results_.size() > 8) image_results_.erase(image_results_.begin());
}

Result<ImageResult> ControlPlane::image_result(JobId id) const {
    std::lock_guard lk(results_mutex_);
    auto it = image_results_.find(id.value);
    if (it == image_results_.end())
        return make_error(ErrorCode::NotFound, "no image result for job");
    return it->second;
}

Result<Submitted> ControlPlane::submit_image(const ImageParams& p, bool require_init) {
    if (!image_engine_)
        return make_error(ErrorCode::Unsupported,
                          "no image engine (stable-diffusion.cpp) is built into this binary");
    if (!image_stream_)
        return make_error(ErrorCode::Unsupported,
                          "the linked image engine does not implement hcs::api::IImageStreamEngine");
    if (auto v = validate_image(p, require_init); !v)
        return v.error();
    auto lora_specs = resolve_loras(p);
    if (!lora_specs)
        return lora_specs.error();
    auto ticket = admit_job(scheduler::WorkKind::Image, p.model,
                            scheduler::estimate_image_job_bytes(p.width, p.height, p.init_image.has_value()));
    if (!ticket)
        return ticket.error();
    return submit_with_channel(
        require_init ? "image.edit" : "image.generate",
        [this, p, require_init, specs = std::move(lora_specs.value()),
         tk = ticket.value()](JobContext& ctx) -> Result<void> {
            (void)tk;  // holds the admission slot until the job closure is destroyed
            if (ctx.cancelled())
                return make_error(ErrorCode::Cancelled, "cancelled before start");
            // One critical section for "switch to the requested model" and "use it" (same reasoning as chat,
            // audit S2).
            std::lock_guard lk(engine_mutex_);
            if (!p.model.empty()) {
                const auto cur = loaded_model();
                if (!cur || *cur != p.model) {
                    if (auto r = load_locked(p.model, ctx.token()); !r)
                        return r;
                }
            }
            std::string id;
            {
                std::lock_guard sl(state_mutex_);
                if (!loaded_)
                    return make_error(ErrorCode::InvalidState,
                                      "no model is loaded: load an image model first or pass a model id");
                if (!loaded_is_image_)
                    return make_error(
                        ErrorCode::Unsupported,
                        "the loaded model is a text/vision model, not an image-generation model");
                id = *loaded_;
            }
            if (ctx.cancelled())
                return make_error(ErrorCode::Cancelled, "cancelled before start");
            active_job_.store(ctx.id().value);
            (void)registry_.transition(id, ModelState::Running);
            ImageRequest ir;
            ir.prompt = p.prompt;
            ir.negative_prompt = p.negative_prompt;
            ir.width = p.width;
            ir.height = p.height;
            ir.steps = p.steps;
            ir.seed = p.seed;
            ir.strength = p.strength;
            ir.cfg_scale = p.cfg_scale;
            ir.preset = p.preset;
            ir.allow_low_quality = p.allow_low_quality;
            ir.init_image = p.init_image;
            ir.loras = specs;
            Result<ImageResult> r = make_error(ErrorCode::Internal, "engine threw");
            try {
                r = require_init ? image_stream_->edit_image(ir, ctx.token())
                                 : image_stream_->generate_image(ir, ctx.token());
            } catch (const std::exception& e) {
                r = error_from_engine_exception(e);
            }
            active_job_.store(0);
            (void)registry_.transition(id, ModelState::Loaded);
            if (!r)
                return r.error();
            if (ctx.cancelled())
                return make_error(ErrorCode::Cancelled, "image generation cancelled");
            ImageResult res = std::move(r.value());
            for (const auto& s : specs) {
                char w[32];
                std::snprintf(w, sizeof(w), "%g", static_cast<double>(s.weight));
                res.loras_applied.push_back(s.id + ":" + w);
            }
            if (p.upscale == "4k" && upscaler_) {
                res.upscale_status = "failed";  // overwritten below on success
                upscale::Options uo;
                refresh_budget();
                uo.memory_budget_bytes = budget_.remaining();
                upscale::Stats st;
                ctx.progress(0.0, "upscaling");
                auto up = upscale::upscale_png(
                    *upscaler_, res.bytes, uo, ctx.token(),
                    [&ctx](double f) { ctx.progress(f, "upscaling"); }, &st);
                if (!up) {
                    if (up.error().code == ErrorCode::Cancelled || ctx.cancelled())
                        return make_error(ErrorCode::Cancelled, "image upscaling cancelled");
                    // The rendered image is kept (a long render must not be lost to an upscale failure); the
                    // response says explicitly that the upscale did not happen and why.
                    res.upscale_detail =
                        up.error().message + " [" + std::string(to_string(up.error().code)) + "]";
                } else {
                    res.upscale_provider = st.provider;
                    res.pre_upscale_width = st.src_w;
                    res.pre_upscale_height = st.src_h;
                    res.upscale_ms = st.model_ms + st.resample_ms;
                    if (st.skipped) {
                        res.upscale_status = "skipped";
                        res.upscale_detail = st.skip_reason;
                    } else {
                        res.upscale_status = "done";
                        res.upscale_tiles = st.tiles;
                        res.upscale_tile_px = st.tile;
                        res.bytes = std::move(up.value());
                        res.width = st.out_w;
                        res.height = st.out_h;
                        res.format = "png";
                    }
                }
            }
            note_image_result(ctx.id(), res);
            return {};
        });
}

}  // namespace hcs::api
