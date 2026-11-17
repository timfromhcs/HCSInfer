#include "hcs/api/control_plane.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>

#include "hcs/api/redact.hpp"
#include "hcs/checked_math.hpp"

namespace hcs::api {
namespace {
using json = nlohmann::json;

constexpr std::uint64_t kKiB = 1024;

// Chat-template control tokens inside user text must reach the tokenizer as plain text, otherwise
// "<|im_end|><|im_start|>system ..." forges a role boundary (M10 audit C3). A zero-width space after the '<'
// keeps the text readable but stops it matching a special token. The engine should also tokenize user text
// with parse_special=false; this is the control-plane side of that defence.
std::string neutralize_control_tokens(const std::string& text) {
    static constexpr std::string_view kMarkers[] = {
        "<|", "<s>", "</s>", "<start_of_turn>", "<end_of_turn>", "<bos>", "<eos>"};
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        out.push_back(text[i]);
        if (text[i] != '<')
            continue;
        for (auto m : kMarkers) {
            if (text.compare(i, m.size(), m) == 0) {
                out += "\xE2\x80\x8B";  // U+200B ZERO WIDTH SPACE
                break;
            }
        }
    }
    return out;
}
}  // namespace

// ---- JobChannel ----------------------------------------------------------------------------
void JobChannel::push(const JobEvent& e) {
    {
        std::lock_guard lk(m_);
        q_.push_back(e);
        if (e.kind == JobEventKind::StateChanged && is_terminal(e.state))
            done_ = true;
    }
    cv_.notify_all();
}

JobChannel::Pop JobChannel::pop(JobEvent& out, std::chrono::steady_clock::time_point deadline) {
    std::unique_lock lk(m_);
    while (true) {
        if (!q_.empty()) {
            out = std::move(q_.front());
            q_.pop_front();
            return Pop::Event;
        }
        if (done_)
            return Pop::Done;
        if (cv_.wait_until(lk, deadline) == std::cv_status::timeout && q_.empty() && !done_)
            return Pop::Timeout;
    }
}

bool JobChannel::done() const {
    std::lock_guard lk(m_);
    return done_;
}

// ---- ControlPlane --------------------------------------------------------------------------
ControlPlane::ControlPlane(ModelRegistry& registry, PathGuard& guard, MemoryBudget& budget, JobQueue& queue,
                           ITextEngine* engine, Logger& logger, ControlPlaneOptions options,
                           IImageEngine* image_engine)
    : registry_(registry),
      guard_(guard),
      budget_(budget),
      queue_(queue),
      engine_(engine),
      stream_(dynamic_cast<ITextStreamEngine*>(engine)),
      image_engine_(image_engine),
      image_stream_(dynamic_cast<IImageStreamEngine*>(image_engine)),
      log_(logger),
      opt_(std::move(options)) {
    refresh_budget();  // first live measurement; later ones happen on every model load
}

// Budget total = measured application headroom (AGENTS.md 10.1) + what this process already holds, so that
// remaining() == the headroom the machine has RIGHT NOW. Without a probe (or when it fails) the total is left
// alone.
void ControlPlane::refresh_budget() const {
    if (!opt_.memory_probe)
        return;
    std::optional<MemoryMeasurements> m;
    try {
        m = opt_.memory_probe();
    } catch (...) {
        return;
    }
    if (!m)
        return;
    std::lock_guard lk(budget_mutex_);
    const std::uint64_t headroom = compute_application_budget(*m);
    const auto total = checked_add(headroom, budget_.reserved());
    budget_.update_budget(total ? *total : UINT64_MAX);
}

std::uint64_t ControlPlane::kv_cache_estimate(std::uint64_t model_bytes,
                                              std::uint32_t context_tokens) noexcept {
    const std::uint64_t per_token = std::clamp<std::uint64_t>(model_bytes / 3500, 96 * kKiB, 512 * kKiB);
    const auto kv = checked_mul(per_token, context_tokens);
    return kv ? *kv : UINT64_MAX;
}

std::vector<ModelView> ControlPlane::models() const {
    std::vector<ModelView> out;
    for (auto& m : registry_.list()) {
        ModelView v;
        v.manifest = m;
        if (auto s = registry_.state(m.id))
            v.state = s.value();
        out.push_back(std::move(v));
    }
    return out;
}

Result<ModelView> ControlPlane::model(const std::string& id) const {
    if (!is_valid_model_id(id))
        return make_error(ErrorCode::InvalidArgument, "invalid model id");
    auto m = registry_.get(id);
    if (!m)
        return m.error();
    ModelView v;
    v.manifest = m.value();
    if (auto s = registry_.state(id))
        v.state = s.value();
    return v;
}

std::optional<std::string> ControlPlane::loaded_model() const {
    std::lock_guard lk(state_mutex_);
    return loaded_;
}

Result<void> ControlPlane::load_model(const std::string& id, const CancellationToken& token) {
    if (!engine_ && !image_engine_)
        return make_error(ErrorCode::Unsupported, "no inference engine is built into this binary");
    std::lock_guard lk(engine_mutex_);
    return load_locked(id, token);
}

Result<void> ControlPlane::unload_locked() {
    std::string id;
    bool is_image = false;
    {
        std::lock_guard lk(state_mutex_);
        if (!loaded_)
            return make_error(ErrorCode::InvalidState, "no model is loaded");
        id = *loaded_;
        is_image = loaded_is_image_;
    }
    (void)registry_.transition(id, ModelState::Unloading);
    IImageEngine* image = is_image ? image_engine_ : nullptr;
    Result<void> r = image ? image->unload() : engine_->unload();
    if (!r) {
        (void)registry_.fail(id, FailureKind::NonRecoverable, r.error().message);
        return r;
    }
    (void)registry_.transition(id, ModelState::Unloaded);
    {
        std::lock_guard lk(state_mutex_);
        loaded_.reset();
        loaded_bytes_.reset();
        loaded_is_image_ = false;
        loaded_has_projector_ = false;
    }
    (void)budget_.release("model:" + id);
    (void)budget_.release("kv:" + id);
    return {};
}

Result<void> ControlPlane::load_locked(const std::string& id, const CancellationToken& token) {
    if (!is_valid_model_id(id))
        return make_error(ErrorCode::InvalidArgument, "invalid model id");
    auto mm = registry_.get(id);
    if (!mm)
        return mm.error();
    const ModelManifest m = mm.value();
    {
        std::lock_guard lk(state_mutex_);
        if (loaded_ && *loaded_ == id)
            return {};
    }
    if (m.kind == ModelKind::Image) {
        if (!image_engine_)
            return make_error(
                ErrorCode::Unsupported,
                "'" + id +
                    "' is an image model but no image engine (stable-diffusion.cpp) is built into "
                    "this binary");
    } else if (!engine_) {
        return make_error(ErrorCode::Unsupported,
                          "no text engine is built into this binary, so text/vision models cannot be loaded");
    }
    if (m.format != ModelFormat::Gguf)
        return make_error(ErrorCode::Unsupported, "the linked engines only load GGUF models (format is " +
                                                      std::string(to_string(m.format)) + ")");
    const bool image_model = m.kind == ModelKind::Image;

    // The path comes from the registry (written by a read-only scan) but is re-checked against
    // the approved roots at load time: a hand-edited registry or a moved link cannot escape.
    auto resolved = guard_.resolve(m.path);
    if (!resolved)
        return resolved.error();

    if (token.cancelled())
        return make_error(ErrorCode::Cancelled, "load cancelled before start");

    // Budget check BEFORE evicting the currently loaded model (AGENTS.md 10.3). The budget is re-measured
    // now, and the estimate covers the weights AND the KV cache of the configured context (M10 audit D4).
    refresh_budget();
    const std::uint64_t kv_bytes = image_model ? 0 : kv_cache_estimate(m.size_bytes, opt_.context_tokens);
    const std::uint64_t need = checked_add(m.size_bytes, kv_bytes).value_or(UINT64_MAX);
    if (budget_.total() != 0) {
        std::uint64_t freed = 0;
        {
            std::lock_guard lk(state_mutex_);
            if (loaded_bytes_)
                freed = *loaded_bytes_;
        }
        const std::uint64_t avail = checked_add(budget_.remaining(), freed).value_or(UINT64_MAX);
        if (need > avail)
            return make_error(
                ErrorCode::OutOfMemory,
                "model needs about " + std::to_string(need) + " bytes (weights " +
                    std::to_string(m.size_bytes) + " + KV cache " + std::to_string(kv_bytes) + " for " +
                    std::to_string(opt_.context_tokens) + " context tokens) but only " +
                    std::to_string(avail) +
                    " are available: unload other models, pick a smaller quantization, reduce the context "
                    "size or use CPU mode");
    }

    if (auto s = registry_.state(id)) {
        const auto st = s.value();
        if (st == ModelState::Discovered || st == ModelState::Unloaded || st == ModelState::Failed) {
            if (auto r = registry_.transition(id, ModelState::Validating); !r)
                return r;
        } else {
            return make_error(ErrorCode::InvalidState, "model is in state " + std::string(to_string(st)));
        }
    }
    if (auto r = registry_.transition(id, ModelState::ReadyToLoad); !r)
        return r;

    bool had_prev = false;
    {
        std::lock_guard lk(state_mutex_);
        had_prev = loaded_.has_value();
    }
    if (had_prev) {
        if (auto r = unload_locked(); !r) {
            (void)registry_.fail(id, FailureKind::Recoverable, "could not unload previous model");
            return r;
        }
    }
    auto release_reservations = [&] {
        (void)budget_.release("model:" + id);
        (void)budget_.release("kv:" + id);
    };
    // ReadyToLoad cannot fail directly (lifecycle table): step back to Validating first so the model is
    // retryable instead of stuck in ReadyToLoad.
    auto fail_before_loading = [&](const std::string& why) {
        (void)registry_.transition(id, ModelState::Validating);
        (void)registry_.fail(id, FailureKind::Recoverable, why);
    };
    if (budget_.total() != 0) {
        std::lock_guard bl(budget_mutex_);
        if (m.size_bytes != 0) {
            if (auto r = budget_.reserve("model:" + id, MemoryCategory::ModelWeights, m.size_bytes); !r) {
                fail_before_loading(r.error().message);
                return r;
            }
        }
        if (kv_bytes != 0) {
            if (auto r = budget_.reserve("kv:" + id, MemoryCategory::KvCache, kv_bytes); !r) {
                release_reservations();
                fail_before_loading(r.error().message);
                return r;
            }
        }
    }
    if (token.cancelled()) {
        release_reservations();
        fail_before_loading("load cancelled");
        return make_error(ErrorCode::Cancelled, "load cancelled before the engine started");
    }
    if (auto r = registry_.transition(id, ModelState::Loading); !r) {
        release_reservations();
        return r;
    }
    LoadRequest lr;
    lr.model_id = id;
    lr.model_path = resolved.value().string();
    lr.backend = opt_.backend;
    lr.context_tokens = opt_.context_tokens;
    if (!image_model) {
        auto pp = projector_path_for(m);  // vision-capable text model: load its mmproj too
        if (!pp) {
            (void)registry_.fail(id, FailureKind::Recoverable, pp.error().message);
            release_reservations();
            return pp.error();
        }
        lr.projector_path = pp.value();
    }
    Result<void> er = make_error(ErrorCode::Internal, "engine threw");
    try {
        er = image_model ? image_engine_->load(lr) : engine_->load(lr);
    } catch (const std::exception& e) {
        er = make_error(ErrorCode::BackendError, std::string("engine exception: ") + e.what());
    }
    if (!er) {
        (void)registry_.fail(id, FailureKind::Recoverable, er.error().message);
        release_reservations();
        log_.warn("control", "model load failed", {{"model", id}, {"code", to_string(er.error().code)}});
        return er;
    }
    (void)registry_.transition(id, ModelState::Loaded);
    {
        std::lock_guard lk(state_mutex_);
        loaded_ = id;
        loaded_bytes_ = need;  // weights + KV estimate: what unloading this model gives back
        loaded_is_image_ = image_model;
        loaded_has_projector_ = !lr.projector_path.empty();
    }
    log_.info("control", "model loaded", {{"model", id}, {"kind", std::string(to_string(m.kind))}});
    return {};
}

Result<void> ControlPlane::unload_model() {
    if (!engine_ && !image_engine_)
        return make_error(ErrorCode::Unsupported, "no inference engine is built into this binary");
    if (const auto j = active_job_.load(); j != 0)
        (void)queue_.cancel(JobId{j});
    std::lock_guard lk(engine_mutex_);
    return unload_locked();
}

Result<JobId> ControlPlane::submit_load(const std::string& id) {
    if (!engine_ && !image_engine_)
        return make_error(ErrorCode::Unsupported, "no inference engine is built into this binary");
    if (!is_valid_model_id(id))
        return make_error(ErrorCode::InvalidArgument, "invalid model id");
    if (auto m = registry_.get(id); !m)
        return m.error();
    return queue_.submit("model.load", [this, id](JobContext& ctx) -> Result<void> {
        if (ctx.cancelled())
            return make_error(ErrorCode::Cancelled, "cancelled before start");
        return load_model(id, ctx.token());
    });
}

// ---- chat ----------------------------------------------------------------------------------
std::string ControlPlane::build_prompt(const std::vector<ChatMessage>& messages, const std::string& tmpl) {
    std::string p;
    if (tmpl == "plain") {
        for (const auto& m : messages) p += m.role + ": " + m.content + "\n";
        p += "assistant:";
        return p;
    }
    for (const auto& m : messages) p += "<|im_start|>" + m.role + "\n" + m.content + "<|im_end|>\n";
    p += "<|im_start|>assistant\n";
    return p;
}

Result<void> ControlPlane::validate_chat(const ChatParams& p) const {
    if (p.messages.empty())
        return make_error(ErrorCode::InvalidArgument, "messages must not be empty");
    if (p.messages.size() > opt_.max_messages)
        return make_error(ErrorCode::InvalidArgument, "too many messages");
    std::size_t total = 0;
    for (const auto& m : p.messages) {
        if (m.role != "system" && m.role != "user" && m.role != "assistant")
            return make_error(ErrorCode::InvalidArgument, "unsupported role '" + m.role + "'");
        total += m.content.size();
        if (total > opt_.max_prompt_bytes)
            return make_error(ErrorCode::InvalidArgument, "prompt exceeds the configured size limit");
    }
    if (p.max_tokens == 0 || p.max_tokens > opt_.max_tokens_cap)
        return make_error(ErrorCode::InvalidArgument,
                          "max_tokens must be in 1.." + std::to_string(opt_.max_tokens_cap));
    if (!(p.temperature >= 0.0f && p.temperature <= 2.0f))
        return make_error(ErrorCode::InvalidArgument, "temperature must be in 0..2");
    if (!p.model.empty() && !is_valid_model_id(p.model))
        return make_error(ErrorCode::InvalidArgument, "invalid model id");
    return validate_vision_chat(p);
}

Result<Submitted> ControlPlane::submit_with_channel(const std::string& name, JobFn fn) {
    auto ch = std::make_shared<JobChannel>();
    auto id = queue_.submit(name, std::move(fn), [ch](const JobEvent& e) { ch->push(e); });
    if (!id)
        return id.error();
    return Submitted{id.value(), ch};
}

void ControlPlane::note_result(JobId id, const TextResult& r) {
    std::lock_guard lk(results_mutex_);
    results_[id.value] = r;
    while (results_.size() > 256) results_.erase(results_.begin());
}

Result<TextResult> ControlPlane::chat_result(JobId id) const {
    std::lock_guard lk(results_mutex_);
    auto it = results_.find(id.value);
    if (it == results_.end())
        return make_error(ErrorCode::NotFound, "no result for job");
    return it->second;
}

Result<Submitted> ControlPlane::submit_chat(const ChatParams& original) {
    if (auto v = validate_chat(original); !v)
        return v.error();
    ChatParams p = original;
    for (auto& m : p.messages) m.content = neutralize_control_tokens(m.content);
    if (chat_has_images(p))
        return submit_vision_chat(p);
    if (!engine_)
        return make_error(ErrorCode::Unsupported, "no inference engine is built into this binary");
    if (!stream_)
        return make_error(
            ErrorCode::Unsupported,
            "the linked text engine does not implement hcs::api::ITextStreamEngine (token streaming)");
    return submit_with_channel("chat.generate", [this, p](JobContext& ctx) -> Result<void> {
        if (ctx.cancelled())
            return make_error(ErrorCode::Cancelled, "cancelled before start");
        // One critical section for "switch to the requested model" and "use it": a concurrent load/unload
        // cannot slip in between and make this job run on a different model (M10 audit S2).
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
                                  "no model is loaded: load one first or pass a model id");
            id = *loaded_;
        }
        if (ctx.cancelled())
            return make_error(ErrorCode::Cancelled, "cancelled before start");
        active_job_.store(ctx.id().value);
        (void)registry_.transition(id, ModelState::Running);
        TextRequest tr;
        {
            const auto& ms = p.messages;
            const bool single_turn = (ms.size() == 1 && ms[0].role == "user") ||
                                     (ms.size() == 2 && ms[0].role == "system" && ms[1].role == "user");
            if (opt_.chat_template == "auto" && single_turn) {
                tr.prompt = ms.back().content;
                if (ms.size() == 2)
                    tr.system_prompt = ms[0].content;
                tr.apply_chat_template = true;
            } else {
                tr.prompt = build_prompt(ms, opt_.chat_template == "plain" ? "plain" : "chatml");
                tr.apply_chat_template = false;
            }
        }
        tr.max_tokens = p.max_tokens;
        tr.temperature = p.temperature;
        tr.seed = p.seed;
        Result<TextResult> r = make_error(ErrorCode::Internal, "engine threw");
        try {
            r = stream_->generate_stream(
                tr,
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

// ---- jobs ----------------------------------------------------------------------------------
std::optional<JobId> ControlPlane::parse_job_id(const std::string& t) {
    if (t.size() < 5 || t.size() > 24 || t.compare(0, 4, "job-") != 0)
        return std::nullopt;
    std::uint64_t v = 0;
    for (std::size_t i = 4; i < t.size(); ++i) {
        if (t[i] < '0' || t[i] > '9')
            return std::nullopt;
        const std::uint64_t d = static_cast<std::uint64_t>(t[i] - '0');
        if (v > (UINT64_MAX - d) / 10)
            return std::nullopt;
        v = v * 10 + d;
    }
    if (v == 0)
        return std::nullopt;
    return JobId{v};
}

Result<JobStatus> ControlPlane::job_status(JobId id) const {
    return queue_.status(id);
}
Result<void> ControlPlane::cancel_job(JobId id) {
    return queue_.cancel(id);
}

json ControlPlane::job_json(const JobStatus& st) const {
    json j;
    j["id"] = to_string(st.id);
    j["name"] = st.name;
    j["state"] = std::string(to_string(st.state));
    j["event_count"] = st.event_count;
    if (st.error)
        j["error"] = {{"code", std::string(to_string(st.error->code))},
                      {"message", redact_paths(st.error->message)}};
    else
        j["error"] = nullptr;
    if (auto r = chat_result(st.id)) {
        j["result"] = {{"finish_reason", r.value().finish_reason},
                       {"prompt_tokens", r.value().prompt_tokens},
                       {"completion_tokens", r.value().completion_tokens},
                       {"ttft_ms", r.value().ttft_ms},
                       {"tokens_per_second", r.value().tokens_per_second}};
    }
    if (auto ir = image_result(st.id)) {
        j["result"] = {{"kind", "image"},
                       {"format", ir.value().format},
                       {"width", ir.value().width},
                       {"height", ir.value().height},
                       {"steps", ir.value().steps},
                       {"bytes", ir.value().bytes.size()},
                       {"generation_ms", ir.value().generation_ms}};
    }
    return j;
}

// ---- introspection -------------------------------------------------------------------------
json ControlPlane::model_json(const ModelView& v) const {
    json j;
    j["id"] = v.manifest.id;
    j["display_name"] = v.manifest.display_name;
    j["format"] = std::string(to_string(v.manifest.format));
    j["kind"] = std::string(to_string(v.manifest.kind));
    j["size_bytes"] = v.manifest.size_bytes;
    j["architecture"] = v.manifest.architecture;
    j["quantization"] = v.manifest.quantization;
    j["license"] = v.manifest.license;
    j["sha256"] = v.manifest.sha256.empty() ? json("unknown") : json(v.manifest.sha256);
    std::string st;  // "ReadyToLoad" -> "ready_to_load"
    for (char c : to_string(v.state)) {
        if (std::isupper(static_cast<unsigned char>(c)) && !st.empty())
            st.push_back('_');
        st.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    j["state"] = st;
    return j;
}

json ControlPlane::runtime_json() const {
    json j;
    j["version"] = opt_.version;
    j["text_engine"] = {{"available", engine_ != nullptr}, {"streaming", stream_ != nullptr}};
    if (engine_) {
        const auto m = engine_->metrics();
        j["text_engine"]["metrics"] = {{"backend", m.backend},
                                       {"device", m.device},
                                       {"load_ms", m.load_ms},
                                       {"ttft_ms", m.ttft_ms},
                                       {"tokens_per_second", m.tokens_per_second},
                                       {"resident_bytes", m.resident_bytes}};
    }
    j["image_engine"] = {{"available", image_engine_ != nullptr}, {"streaming", image_stream_ != nullptr}};
    if (image_engine_) {
        const auto m = image_engine_->metrics();
        j["image_engine"]["metrics"] = {
            {"backend", m.backend},         {"device", m.device},
            {"load_ms", m.load_ms},         {"image_generation_ms", m.image_generation_ms},
            {"image_steps", m.image_steps}, {"resident_bytes", m.resident_bytes}};
    } else {
        j["image_engine"]["reason"] = "no image engine (stable-diffusion.cpp) is built into this binary";
    }
    const auto lm = loaded_model();
    j["loaded_model"] = lm ? json(*lm) : json(nullptr);
    j["jobs_pending"] = queue_.pending();
    j["uptime_s"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
    j["chat_template"] = opt_.chat_template;
    return j;
}

json ControlPlane::devices_json() const {
    std::lock_guard lk(devices_mutex_);
    if (!devices_cache_) {
        json j;
        if (opt_.devices_provider) {
            try {
                j = opt_.devices_provider();
            } catch (const std::exception& e) {
                j = {{"status", "unknown"}, {"reason", std::string("hardware report failed: ") + e.what()}};
            }
        } else {
            j = {{"status", "unknown"}, {"reason", "hardware module is not built into this binary"}};
        }
        devices_cache_ = std::move(j);
    }
    return *devices_cache_;
}

json ControlPlane::memory_json() const {
    json j;
    refresh_budget();
    const auto total = budget_.total();
    j["budget_known"] = total != 0;
    if (total == 0) {
        j["total_bytes"] = "unknown";
        j["remaining_bytes"] = "unknown";
        j["note"] =
            "no live memory measurement is available in this build; model-size budget checks are skipped";
    } else {
        j["total_bytes"] = total;
        j["remaining_bytes"] = budget_.remaining();
    }
    if (opt_.memory_probe) {
        try {
            if (auto m = opt_.memory_probe()) {
                j["measured"] = {
                    {"available_physical_bytes", m->available_physical_bytes},
                    {"os_reserve_bytes", m->os_reserve_bytes},
                    {"safety_margin_bytes", m->safety_margin_bytes},
                    {"application_headroom_bytes", compute_application_budget(*m)},
                    {"vulkan_headroom_bytes",
                     m->vulkan_headroom_bytes ? json(*m->vulkan_headroom_bytes) : json("unknown")}};
            }
        } catch (...) {
        }
    }
    j["reserved_bytes"] = budget_.reserved();
    json es = json::array();
    for (const auto& e : budget_.entries())
        es.push_back({{"key", e.key}, {"category", std::string(to_string(e.category))}, {"bytes", e.bytes}});
    j["entries"] = std::move(es);
    return j;
}

}  // namespace hcs::api
