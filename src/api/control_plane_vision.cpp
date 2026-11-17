// ControlPlane: image understanding (chat messages carrying an image). Kept in its own translation unit so
// the text/image paths in control_plane.cpp stay untouched by the vision feature.
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <regex>

#include "hcs/api/control_plane.hpp"
#include "hcs/api/image_probe.hpp"

namespace hcs::api {
namespace {

namespace fs = std::filesystem;

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Removes trailing quantisation/precision tokens ("-Q8_0", "-f16", "-bf16", "_q4_k_m", ...) repeatedly.
std::string strip_quant(std::string s) {
    static const std::regex quant(R"([-_.](q\d\w*|iq\d\w*|f16|f32|bf16|fp16|fp32)$)");
    for (int i = 0; i < 4; ++i) {
        std::string next = std::regex_replace(s, quant, "");
        if (next == s)
            break;
        s = std::move(next);
    }
    return s;
}

std::string trim_seps(std::string s) {
    const auto is_sep = [](char c) { return c == '-' || c == '_' || c == '.' || c == ' '; };
    while (!s.empty() && is_sep(s.front())) s.erase(s.begin());
    while (!s.empty() && is_sep(s.back())) s.pop_back();
    return s;
}

std::string norm_model_stem(const std::string& path) {
    return strip_quant(lower(fs::path(path).stem().string()));
}

std::string norm_projector_stem(const std::string& path) {
    std::string s = lower(fs::path(path).stem().string());
    for (std::size_t p; (p = s.find("mmproj")) != std::string::npos;) s.erase(p, 6);
    return strip_quant(trim_seps(s));
}

bool is_projector(const ModelManifest& m) {
    return lower(m.architecture) == "clip" ||
           lower(fs::path(m.path).filename().string()).rfind("mmproj", 0) == 0;
}

std::string parent_key(const std::string& path) {
    return lower(fs::path(path).parent_path().lexically_normal().string());
}

}  // namespace

bool ControlPlane::chat_has_images(const ChatParams& p) {
    return std::any_of(p.messages.begin(), p.messages.end(),
                       [](const ChatMessage& m) { return !m.images.empty(); });
}

bool ControlPlane::has_vision_engine() const noexcept {
    return dynamic_cast<IVisionStreamEngine*>(engine_) != nullptr;
}

bool ControlPlane::loaded_has_vision() const {
    std::lock_guard lk(state_mutex_);
    return loaded_ && !loaded_is_image_ && loaded_has_projector_;
}

Result<std::string> ControlPlane::projector_path_for(const ModelManifest& m) const {
    if (is_projector(m))
        return std::string();  // an mmproj is not a vision model on its own
    const std::string dir = parent_key(m.path);
    std::vector<ModelManifest> projectors;
    std::size_t other_models = 0;  // non-projector, non-image models in the same folder (m included)
    for (const auto& r : registry_.list()) {
        if (parent_key(r.path) != dir || r.kind == ModelKind::Image)
            continue;
        if (is_projector(r))
            projectors.push_back(r);
        else
            ++other_models;
    }
    if (projectors.empty())
        return std::string();
    const std::string want = norm_model_stem(m.path);
    const ModelManifest* pick = nullptr;
    for (const auto& pr : projectors) {
        if (norm_projector_stem(pr.path) == want) {
            pick = &pr;
            break;
        }
    }
    if (!pick && projectors.size() == 1 && other_models == 1)
        pick = &projectors.front();
    if (!pick)
        return std::string();
    auto resolved = guard_.resolve(pick->path);  // approved roots only, like the model itself
    if (!resolved)
        return resolved.error();
    return resolved.value().string();
}

Result<void> ControlPlane::validate_vision_chat(const ChatParams& p) const {
    if (!chat_has_images(p))
        return {};
    std::size_t n_images = 0;
    for (const auto& m : p.messages) {
        if (!m.images.empty() && m.role != "user")
            return make_error(ErrorCode::InvalidArgument, "images are only allowed in user messages");
        n_images += m.images.size();
    }
    if (n_images != 1)
        return make_error(ErrorCode::InvalidArgument, "exactly one image per request is supported (got " +
                                                          std::to_string(n_images) + ")");
    const auto& ms = p.messages;
    const bool shape_ok = (ms.size() == 1 && ms[0].role == "user") ||
                          (ms.size() == 2 && ms[0].role == "system" && ms[1].role == "user");
    if (!shape_ok)
        return make_error(
            ErrorCode::InvalidArgument,
            "an image is supported in a single user message, optionally preceded by one system message");
    if (ms.back().content.empty())
        return make_error(ErrorCode::InvalidArgument, "a text prompt is required together with the image");
    const auto& img = ms.back().images.front();
    if (img.bytes.empty())
        return make_error(ErrorCode::InvalidArgument, "image is empty");
    if (img.bytes.size() > opt_.max_image_bytes)
        return make_error(ErrorCode::InvalidArgument,
                          "image exceeds the " + std::to_string(opt_.max_image_bytes) + " byte limit");
    const auto probe = probe_image(img.bytes.data(), img.bytes.size());
    if (!probe)
        return make_error(ErrorCode::InvalidArgument,
                          "the image is not a recognised png/jpeg/gif/bmp (or its header is truncated)");
    if (probe->pixels() > opt_.max_image_pixels)
        return make_error(ErrorCode::InvalidArgument,
                          "image dimensions " + std::to_string(probe->width) + "x" +
                              std::to_string(probe->height) + " exceed the " +
                              std::to_string(opt_.max_image_pixels) + " pixel limit");
    return {};
}

Result<Submitted> ControlPlane::submit_vision_chat(const ChatParams& p) {
    if (!engine_)
        return make_error(ErrorCode::Unsupported, "no inference engine is built into this binary");
    auto* vis = dynamic_cast<IVisionStreamEngine*>(engine_);
    if (!vis)
        return make_error(
            ErrorCode::Unsupported,
            "the linked text engine does not implement hcs::api::IVisionStreamEngine (image input)");
    return submit_with_channel("vision.analyze", [this, p, vis](JobContext& ctx) -> Result<void> {
        if (ctx.cancelled())
            return make_error(ErrorCode::Cancelled, "cancelled before start");
        if (!p.model.empty()) {
            const auto cur = loaded_model();
            if (!cur || *cur != p.model) {
                if (auto r = load_model(p.model); !r)
                    return r;
            }
        }
        std::lock_guard lk(engine_mutex_);
        std::string id;
        bool has_projector = false;
        {
            std::lock_guard sl(state_mutex_);
            if (!loaded_)
                return make_error(ErrorCode::InvalidState,
                                  "no model is loaded: load one first or pass a model id");
            if (loaded_is_image_)
                return make_error(ErrorCode::Unsupported,
                                  "the loaded model is an image-generation model, not a vision model");
            id = *loaded_;
            has_projector = loaded_has_projector_;
        }
        if (!has_projector)
            return make_error(ErrorCode::Unsupported,
                              "model '" + id +
                                  "' has no vision projector: place its mmproj*.gguf GGUF next to the model "
                                  "inside an approved root, rescan, and reload");
        if (ctx.cancelled())
            return make_error(ErrorCode::Cancelled, "cancelled before start");
        active_job_.store(ctx.id().value);
        (void)registry_.transition(id, ModelState::Running);
        VisionRequest vr;
        {
            const auto& ms = p.messages;
            vr.text.prompt = ms.back().content;
            if (ms.size() == 2)
                vr.text.system_prompt = ms[0].content;
            vr.text.apply_chat_template = true;
            vr.text.max_tokens = p.max_tokens;
            vr.text.temperature = p.temperature;
            vr.text.seed = p.seed;
            vr.image = ms.back().images.front();
        }
        Result<TextResult> r = make_error(ErrorCode::Internal, "engine threw");
        try {
            r = vis->analyze_image_stream(
                vr,
                [&ctx](std::string_view piece) {
                    if (ctx.cancelled())
                        return false;
                    ctx.output(std::string(piece));
                    return !ctx.cancelled();
                },
                ctx.token());
        } catch (const std::exception& e) {
            r = make_error(ErrorCode::BackendError, std::string("engine exception: ") + e.what());
        }
        active_job_.store(0);
        (void)registry_.transition(id, ModelState::Loaded);
        if (!r)
            return r.error();
        note_result(ctx.id(), r.value());
        if (ctx.cancelled() || r.value().finish_reason == "cancelled")
            return make_error(ErrorCode::Cancelled, "generation cancelled");
        return {};
    });
}

}  // namespace hcs::api
