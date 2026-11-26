// ControlPlane: image generation / editing (text-to-image and img2img through IImageStreamEngine). Kept in
// its own translation unit like control_plane_vision.cpp. Everything an HTTP/MCP caller can influence is
// validated here BEFORE the engine is touched: dimensions, pixel budget, steps, strength, prompt size and the
// init image header (format + decoded pixel count are probed without decoding, AGENTS.md 19.3).
#include <algorithm>

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
    auto ticket = admit_job(scheduler::WorkKind::Image, p.model,
                            scheduler::estimate_image_job_bytes(p.width, p.height, p.init_image.has_value()));
    if (!ticket)
        return ticket.error();
    return submit_with_channel(
        require_init ? "image.edit" : "image.generate",
        [this, p, require_init, tk = ticket.value()](JobContext& ctx) -> Result<void> {
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
            note_image_result(ctx.id(), r.value());
            return {};
        });
}

}  // namespace hcs::api
