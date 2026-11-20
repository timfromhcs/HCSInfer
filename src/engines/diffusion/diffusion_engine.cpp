// stable-diffusion.cpp image engine adapter. Only this translation unit may include
// stable-diffusion.h / stb headers; upstream types never leave this module.
#include "hcs/engines/diffusion_engine.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "stable-diffusion.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace hcs::engines {

namespace {

using Clock = std::chrono::steady_clock;
using api::ImageResult;

constexpr std::uint32_t kMinDimension = 64;
constexpr std::uint32_t kMaxDimension = 4096;
constexpr std::uint32_t kMaxSteps = 150;
constexpr std::size_t kMaxEncodedImageBytes = 32u << 20;  // edit input limit
constexpr std::uint64_t kMaxDecodedPixels = 16u << 20;

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// ---------------------------------------------------------------------------------------------
// Process-wide capture of stable-diffusion.cpp log output (device + load evidence, error context).
// ---------------------------------------------------------------------------------------------
class LogSink {
public:
    static LogSink& instance() {
        static LogSink s;
        return s;
    }
    void append(const char* text) {
        if (!text)
            return;
        if (echo_)
            std::fputs(text, stderr);
        std::lock_guard l(m_);
        buf_ += text;
        constexpr std::size_t kMax = std::size_t{256} * 1024;
        if (buf_.size() > kMax) {
            const std::size_t drop = buf_.size() - kMax / 2;
            buf_.erase(0, drop);
            base_ += drop;
        }
    }
    std::uint64_t mark() const {
        std::lock_guard l(m_);
        return base_ + buf_.size();
    }
    std::string since(std::uint64_t mark) const {
        std::lock_guard l(m_);
        if (mark < base_)
            mark = base_;
        const std::size_t off = static_cast<std::size_t>(mark - base_);
        return off >= buf_.size() ? std::string() : buf_.substr(off);
    }

private:
    LogSink() { echo_ = std::getenv("HCS_DIFFUSION_VERBOSE") != nullptr; }
    mutable std::mutex m_;
    std::string buf_;
    std::uint64_t base_ = 0;
    bool echo_ = false;
};

void log_cb(sd_log_level_t, const char* text, void*) {
    LogSink::instance().append(text);
}

// stable-diffusion.cpp draws its own progress bars/percentage lines on stdout unless a legacy
// progress callback is registered (see upstream src/core/util.cpp: print_progress_line). That would
// corrupt `HCSInfer image --json` and the MCP stdio JSON-RPC stream, so a no-op callback is always
// installed. Per-job progress keeps flowing through sd_set_generation_progress_callback instead.
bool verbose_enabled() {
    static const bool v = std::getenv("HCS_DIFFUSION_VERBOSE") != nullptr;
    return v;
}

void legacy_progress_cb(int step, int steps, float time, void*) {
    if (!verbose_enabled())
        return;
    std::fprintf(stderr, "[diffusion] step %d/%d %.3fs\n", step, steps, static_cast<double>(time));
}

void ensure_log_capture() {
    static std::once_flag once;
    std::call_once(once, [] {
        sd_set_log_callback(log_cb, nullptr);
        sd_set_progress_callback(legacy_progress_cb, nullptr);
    });
}

std::string tail_error_lines(const std::string& log) {
    std::vector<std::string> keep;
    std::size_t pos = 0;
    while (pos < log.size()) {
        std::size_t e = log.find('\n', pos);
        if (e == std::string::npos)
            e = log.size();
        std::string line = log.substr(pos, e - pos);
        pos = e + 1;
        const std::string low = lower(line);
        if (low.find("using upstream ggml") != std::string::npos)
            continue;  // startup warning that happens to contain the word "unsupported"
        if (low.find("error") != std::string::npos || low.find("failed") != std::string::npos ||
            low.find("unsupported") != std::string::npos || low.find("invalid") != std::string::npos)
            keep.push_back(line);
    }
    std::string out;
    const std::size_t from = keep.size() > 3 ? keep.size() - 3 : 0;
    for (std::size_t i = from; i < keep.size(); ++i) out += (out.empty() ? "" : " | ") + keep[i];
    if (out.size() > 500)
        out.resize(500);
    return out;
}

Error last_error(ErrorCode fallback, const std::string& what) {
    const char* e = sd_get_last_error();
    std::string msg = what;
    if (e && *e) {
        msg += ": ";
        msg += e;
        if (msg.size() > 900)
            msg.resize(900);
    }
    return Error(fallback, msg);
}

Error map_status(sd_status_t st, const std::string& what) {
    switch (st) {
        case SD_STATUS_OK:
            return Error(ErrorCode::Internal, what);
        case SD_STATUS_INVALID_ARGUMENT:
        case SD_STATUS_UNKNOWN_KEY:
        case SD_STATUS_TYPE_MISMATCH:
        case SD_STATUS_OUT_OF_RANGE:
            return last_error(ErrorCode::InvalidArgument, what);
        case SD_STATUS_UNSUPPORTED:
            return last_error(ErrorCode::Unsupported, what);
        case SD_STATUS_OUT_OF_MEMORY:
            return last_error(ErrorCode::OutOfMemory, what);
        case SD_STATUS_LOAD_FAILED:
            return last_error(ErrorCode::ModelError, what);
        case SD_STATUS_GENERATION_FAILED:
            return last_error(ErrorCode::BackendError, what);
        case SD_STATUS_BUSY:
            return last_error(ErrorCode::Busy, what);
        case SD_STATUS_CANCELLED:
            return Error(ErrorCode::Cancelled, what);
    }
    return last_error(ErrorCode::BackendError, what);
}

bool is_cpu_device(const std::string& name) {
    return lower(name) == "cpu";
}

struct DeviceEntry {
    std::string name;
    std::string description;
    bool cpu = false;
};

std::vector<DeviceEntry> device_entries() {
    std::vector<DeviceEntry> out;
    const std::size_t need = sd_list_devices(nullptr, 0);
    if (need == 0)
        return out;
    std::string buf(need + 1, '\0');
    const std::size_t written = sd_list_devices(buf.data(), buf.size());
    if (written == 0)
        return out;
    buf.resize(written);
    std::size_t pos = 0;
    while (pos < buf.size()) {
        std::size_t e = buf.find('\n', pos);
        if (e == std::string::npos)
            e = buf.size();
        std::string line = buf.substr(pos, e - pos);
        pos = e + 1;
        while (!line.empty() && (line.back() == '\r')) line.pop_back();
        if (line.empty())
            continue;
        DeviceEntry d;
        const std::size_t tab = line.find('\t');
        d.name = line.substr(0, tab);
        if (tab != std::string::npos)
            d.description = line.substr(tab + 1);
        d.cpu = is_cpu_device(d.name);
        out.push_back(std::move(d));
    }
    return out;
}

// 0-terminated RGB copy of a PNG/JPEG input, bounded before decoding (untrusted input).
struct DecodedImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgb;
};

Result<DecodedImage> decode_input(const ImageInput& in) {
    if (in.bytes.empty())
        return Error(ErrorCode::InvalidArgument, "input image is empty");
    if (in.bytes.size() > kMaxEncodedImageBytes)
        return Error(ErrorCode::SecurityViolation, "input image exceeds 32 MiB");
    if (in.bytes.size() > static_cast<std::size_t>(INT32_MAX))
        return Error(ErrorCode::InvalidArgument, "input image too large to decode");
    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(in.bytes.data(), static_cast<int>(in.bytes.size()), &w, &h, &comp))
        return Error(ErrorCode::InvalidArgument, "input image is not a supported PNG/JPEG");
    if (w <= 0 || h <= 0)
        return Error(ErrorCode::InvalidArgument, "input image has invalid dimensions");
    if (static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(h) > kMaxDecodedPixels)
        return Error(ErrorCode::SecurityViolation, "input image exceeds 16 megapixels");
    DecodedImage out;
    stbi_uc* px = stbi_load_from_memory(in.bytes.data(), static_cast<int>(in.bytes.size()), &w, &h, &comp, 3);
    if (!px)
        return Error(ErrorCode::InvalidArgument, "input image could not be decoded");
    out.width = w;
    out.height = h;
    out.rgb.assign(px, px + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3);
    stbi_image_free(px);
    return out;
}

void png_sink(void* ctx, void* data, int size) {
    auto* out = static_cast<std::vector<std::uint8_t>*>(ctx);
    const auto* p = static_cast<const std::uint8_t*>(data);
    out->insert(out->end(), p, p + size);
}

struct ProgressState {
    const CancellationToken* token = nullptr;
    sd_ctx_t* ctx = nullptr;
    const DiffusionImageEngine::ProgressSink* sink = nullptr;
};

void progress_cb(const sd_generation_progress_t* p, void* data) {
    auto* st = static_cast<ProgressState*>(data);
    if (!st)
        return;
    if (st->token && st->token->cancelled()) {
        sd_cancel_generation(st->ctx, SD_CANCEL_ALL);
        return;
    }
    // The pointer is non-null even when the caller passed an empty std::function (the blocking entry points
    // do): calling that throws std::bad_function_call out of the C callback and failed every blocking
    // generation.
    if (!st->sink || !*st->sink || !p)
        return;
    double frac = 0.0;
    if (p->total > 0)
        frac = static_cast<double>(p->completed) / static_cast<double>(p->total);
    std::string msg = p->stage ? p->stage : "generating";
    if (p->total > 0)
        msg += " " + std::to_string(p->completed) + "/" + std::to_string(p->total);
    if (std::isfinite(frac))
        (*st->sink)(std::clamp(frac, 0.0, 1.0), std::move(msg));
}

// ---------------------------------------------------------------------------------------------
// Multi-file models (FLUX.3 Klein = diffusion GGUF + LLM text encoder + VAE).
// ---------------------------------------------------------------------------------------------
struct Components {
    std::string llm;
    std::string vae;
    std::string params_backend;
    bool any() const { return !llm.empty() || !vae.empty(); }
};

// `<model without extension>.components.json`: {"llm": "rel/path", "vae": "rel/path", "params_backend":
// "cpu"}. Paths must be relative to the model's directory, free of "..", and stay under that directory after
// canonicalisation (no junction/symlink escape). The file is untrusted input and size-capped.
Result<Components> read_sidecar(const std::filesystem::path& model) {
    namespace fs = std::filesystem;
    fs::path side = model;
    side.replace_extension(".components.json");
    std::error_code ec;
    if (!fs::is_regular_file(side, ec))
        return Components{};
    const auto size = fs::file_size(side, ec);
    if (ec || size == 0 || size > (64u << 10))
        return Error(ErrorCode::InvalidArgument,
                     "components sidecar is empty or larger than 64 KiB: " + side.filename().string());
    std::ifstream f(side, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    f.read(text.data(), static_cast<std::streamsize>(text.size()));
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object())
        return Error(ErrorCode::InvalidArgument, "components sidecar is not a JSON object");
    const fs::path base = fs::weakly_canonical(model.parent_path(), ec);
    auto resolve = [&](const char* key, std::string& out) -> Result<void> {
        if (!j.contains(key))
            return {};
        if (!j[key].is_string())
            return Error(ErrorCode::InvalidArgument,
                         std::string("components sidecar: '") + key + "' must be a string");
        const std::string rel = j[key].get<std::string>();
        const fs::path rp(rel);
        if (rel.empty() || rp.is_absolute() || rp.has_root_name() || rel.find("..") != std::string::npos)
            return Error(ErrorCode::SecurityViolation, std::string("components sidecar: '") + key +
                                                           "' must be a relative path without '..'");
        std::error_code e2;
        const fs::path full = fs::weakly_canonical(base / rp, e2);
        if (e2 || !fs::is_regular_file(full, e2))
            return Error(ErrorCode::NotFound,
                         std::string("components sidecar: '") + key + "' file not found");
        const auto rel_to = full.lexically_relative(base);
        if (rel_to.empty() || *rel_to.begin() == "..")
            return Error(ErrorCode::SecurityViolation,
                         std::string("components sidecar: '") + key + "' escapes the model directory");
        out = full.string();
        return {};
    };
    Components c;
    if (auto r = resolve("llm", c.llm); !r)
        return r.error();
    if (auto r = resolve("vae", c.vae); !r)
        return r.error();
    if (j.contains("params_backend") && j["params_backend"].is_string())
        c.params_backend = j["params_backend"].get<std::string>();
    return c;
}

// Quality/time points per model family. Values come from the step sweep in docs/test-evidence/flux3
// (distilled FLUX.3 Klein: 16 steps is the upstream reference, cfg 1.0).
struct PresetPoint {
    std::uint32_t steps;
    float cfg;
};

// Measured on Radeon 680M (docs/test-evidence/wave-f, single noisy samples): 6 steps matches 8 steps on PSNR
// vs the 16-step render (24.1 dB each; DSSIM 0.091 vs 0.077) and is ~23 % faster than 16 steps (~9 % faster
// than 8); 4 steps drops to 20.3 dB and was not faster in this sample.
constexpr PresetPoint kFlux3Fast{6, 1.0f};
constexpr PresetPoint kFlux3Quality{16, 1.0f};

std::optional<PresetPoint> preset_for(const std::string& family, const std::string& preset) {
    if (family == "flux3") {
        if (preset == "fast")
            return kFlux3Fast;
        if (preset == "quality")
            return kFlux3Quality;
    }
    return std::nullopt;
}

bool valid_preset_name(const std::string& p) {
    return p.empty() || p == "fast" || p == "quality";
}

std::string family_from_log(const std::string& log) {
    const std::string low = lower(log);
    if (low.find("flux.3") != std::string::npos || low.find("flux3") != std::string::npos)
        return "flux3";
    return {};
}

struct Session {
    sd_ctx_t* ctx = nullptr;
    std::string device_name;
    std::string device_description;
    std::string family;  // "flux3" | "" (unknown); drives presets
    bool gpu = false;
    std::uint64_t model_bytes = 0;
    std::mutex run_mu;
    ~Session() {
        if (ctx)
            sd_ctx_free(ctx);
    }
};

struct RunOutcome {
    ImageResult result;
    std::string log;
};

}  // namespace

// ---------------------------------------------------------------------------------------------
struct DiffusionImageEngine::Impl {
    explicit Impl(JobQueue& q) : queue(q) {}

    JobQueue& queue;
    mutable std::mutex mu;
    std::shared_ptr<Session> session;
    std::vector<JobId> jobs;
    bool loading = false;
    bool unloading = false;

    mutable std::mutex mm;
    EngineMetrics metrics;
    std::string load_log;

    mutable std::mutex rm;
    std::map<JobId, ImageResult> results;

    Result<ImageResult> render(sd_ctx_t* ctx, const std::string& family, const ImageRequest& req,
                               const CancellationToken& token, const ProgressSink& sink);
};

DiffusionImageEngine::DiffusionImageEngine(JobQueue& queue) : impl_(std::make_unique<Impl>(queue)) {}
DiffusionImageEngine::~DiffusionImageEngine() {
    (void)unload();
}

bool DiffusionImageEngine::vulkan_compiled() {
#ifdef HCS_DIFFUSION_VULKAN
    return true;
#else
    return false;
#endif
}

std::vector<std::string> DiffusionImageEngine::devices() {
    std::vector<std::string> out;
    for (const auto& d : device_entries()) out.push_back(d.name + "\t" + d.description);
    return out;
}

std::string DiffusionImageEngine::upstream_version() {
    const char* v = sd_version();
    return v ? v : "unknown";
}

bool DiffusionImageEngine::loaded() const {
    std::lock_guard l(impl_->mu);
    return impl_->session != nullptr;
}

EngineMetrics DiffusionImageEngine::metrics() const {
    std::lock_guard l(impl_->mm);
    return impl_->metrics;
}

std::string DiffusionImageEngine::last_load_log() const {
    std::lock_guard l(impl_->mm);
    return impl_->load_log;
}

Result<void> DiffusionImageEngine::load(const LoadRequest& req) {
    ensure_log_capture();
    {
        std::lock_guard l(impl_->mu);
        if (impl_->session || impl_->loading || impl_->unloading)
            return Error(ErrorCode::InvalidState,
                         "a model is already loaded or a load/unload is in progress; unload first");
        impl_->loading = true;
    }
    struct LoadingGuard {
        Impl& i;
        ~LoadingGuard() {
            std::lock_guard l(i.mu);
            i.loading = false;
        }
    } guard{*impl_};

    if (req.model_path.empty())
        return Error(ErrorCode::InvalidArgument, "model_path is empty");
    if (!req.projector_path.empty())
        return Error(ErrorCode::InvalidArgument, "a diffusion model does not take a projector file");
    if (req.threads < 0 || req.threads > 1024)
        return Error(ErrorCode::InvalidArgument, "threads out of range");

    bool mmap = false;
    if (!req.load_mode.empty() && req.load_mode != "auto") {
        if (req.load_mode == "mmap") {
            mmap = true;
        } else if (req.load_mode == "none") {
            mmap = false;
        } else {
            return Error(ErrorCode::Unsupported,
                         "load_mode '" + req.load_mode +
                             "' is not supported by the diffusion adapter (auto|mmap|none)");
        }
    }

    std::uint64_t total_bytes = 0;
    {
        std::error_code ec;
        const auto size = std::filesystem::file_size(req.model_path, ec);
        if (ec || size == 0)
            return Error(ErrorCode::NotFound, "cannot read model file: " + req.model_path);
        total_bytes = size;
    }

    Components comp;
    comp.llm = req.llm_path;
    comp.vae = req.vae_path;
    comp.params_backend = req.params_backend;
    if (!comp.any()) {
        auto side = read_sidecar(std::filesystem::path(req.model_path));
        if (!side)
            return side.error();
        comp = side.value();
        if (!req.params_backend.empty())
            comp.params_backend = req.params_backend;
    }
    if (!comp.params_backend.empty() && comp.params_backend != "cpu" && comp.params_backend != "disk")
        return Error(ErrorCode::InvalidArgument, "params_backend must be 'cpu' or 'disk'");
    for (const std::string* extra : {&comp.llm, &comp.vae}) {
        if (extra->empty())
            continue;
        std::error_code ec;
        const auto size = std::filesystem::file_size(*extra, ec);
        if (ec || size == 0)
            return Error(ErrorCode::NotFound, "cannot read model component file: " + *extra);
        total_bytes += size;
    }

    const auto entries = device_entries();
    if (entries.empty())
        return Error(ErrorCode::BackendError, "stable-diffusion.cpp reports no compute devices");
    const DeviceEntry* chosen = nullptr;
    for (const auto& d : entries) {
        if (d.cpu)
            continue;
        if (!req.device.empty() && lower(d.name) != lower(req.device) &&
            lower(d.description).find(lower(req.device)) == std::string::npos)
            continue;
        chosen = &d;
        break;
    }
    bool use_gpu = false;
    switch (req.backend) {
        case Backend::Cpu:
            break;
        case Backend::Vulkan:
            if (!vulkan_compiled())
                return Error(ErrorCode::Unsupported,
                             "Vulkan requested but this build has no Vulkan backend (HCS_ENABLE_VULKAN=OFF)");
            if (!chosen)
                return Error(ErrorCode::BackendError,
                             "Vulkan requested but no GPU device is available" +
                                 (req.device.empty() ? std::string() : " matching '" + req.device + "'"));
            use_gpu = true;
            break;
        case Backend::Auto:
            use_gpu = chosen != nullptr;
            break;
    }
    if (!use_gpu) {
        chosen = nullptr;
        for (const auto& d : entries) {
            if (d.cpu) {
                chosen = &d;
                break;
            }
        }
        if (!chosen)
            return Error(ErrorCode::BackendError, "no CPU device reported by stable-diffusion.cpp");
    }

    const std::uint64_t log_mark = LogSink::instance().mark();
    const auto t0 = Clock::now();
    sd_ctx_opt_t* opt = sd_ctx_opt_create();
    if (!opt)
        return Error(ErrorCode::OutOfMemory, "could not allocate stable-diffusion context options");
    auto free_opt = [&] { sd_opt_free(opt); };
    auto set_string = [&](const char* key, const std::string& value) -> Result<void> {
        const sd_status_t st = sd_opt_set_string(opt, key, value.c_str());
        if (st != SD_STATUS_OK)
            return map_status(st, std::string("could not set '") + key + "'");
        return {};
    };
    auto set_int = [&](const char* key, std::int64_t value) -> Result<void> {
        const sd_status_t st = sd_opt_set_int(opt, key, value);
        if (st != SD_STATUS_OK)
            return map_status(st, std::string("could not set '") + key + "'");
        return {};
    };

    // Multi-file models (FLUX.3 Klein) name the DiT as diffusion_model_path; all-in-one checkpoints use
    // model_path.
    Result<void> cfg = set_string(comp.any() ? "diffusion_model_path" : "model_path", req.model_path);
    if (cfg && !comp.llm.empty())
        cfg = set_string("llm_path", comp.llm);
    if (cfg && !comp.vae.empty())
        cfg = set_string("vae_path", comp.vae);
    if (cfg)
        cfg = set_string("backend", chosen->name);
    if (cfg)
        cfg = set_string("params_backend",
                         comp.params_backend.empty() ? std::string("cpu") : comp.params_backend);
    // HCS LoadRequest::threads == 0 means "engine default"; upstream rejects 0, so only send a positive
    // value.
    if (cfg && req.threads > 0)
        cfg = set_int("n_threads", req.threads);
    if (cfg && !req.load_mode.empty() && req.load_mode != "auto") {
        const sd_status_t st = sd_opt_set_bool(opt, "enable_mmap", mmap);
        if (st != SD_STATUS_OK)
            cfg = map_status(st, "could not set 'enable_mmap'");
    }
    if (!cfg) {
        free_opt();
        return cfg.error();
    }

    sd_ctx_t* ctx = nullptr;
    const sd_status_t created = sd_ctx_create(opt, &ctx);
    free_opt();
    const std::string log = LogSink::instance().since(log_mark);
    if (created != SD_STATUS_OK || !ctx) {
        {
            std::lock_guard l(impl_->mm);
            impl_->load_log = log;
        }
        std::string why = tail_error_lines(log);
        if (!comp.any() && log.find("get sd version from file failed") != std::string::npos)
            why += (why.empty() ? "" : " | ") +
                   std::string(
                       "this file is not an all-in-one checkpoint; a diffusion-only model (FLUX.3 Klein) "
                       "needs its text encoder and VAE: pass llm_path/vae_path (CLI --llm/--vae) or add a "
                       "<model>.components.json sidecar");
        Error e = map_status(created, "stable-diffusion.cpp could not load the model" +
                                          (why.empty() ? std::string() : ": " + why));
        return e;
    }

    auto s = std::make_shared<Session>();
    s->ctx = ctx;
    s->device_name = chosen->name;
    s->device_description = chosen->description;
    s->gpu = use_gpu;
    s->family = family_from_log(log);

    EngineMetrics m;
    m.backend = use_gpu ? "vulkan" : "cpu";
    m.device = chosen->description.empty() ? chosen->name : chosen->description + " (" + chosen->name + ")";
    m.load_ms = ms_since(t0);
    s->model_bytes = total_bytes;
    m.resident_bytes = s->model_bytes;  // upper bound (sum of component file sizes); no RSS is reported
    {
        std::lock_guard l(impl_->mm);
        impl_->metrics = m;
        impl_->load_log = log;
    }
    {
        std::lock_guard l(impl_->mu);
        impl_->session = std::move(s);
    }
    return {};
}

Result<void> DiffusionImageEngine::unload() {
    std::shared_ptr<Session> s;
    std::vector<JobId> jobs;
    {
        std::lock_guard l(impl_->mu);
        if (!impl_->session)
            return {};  // idempotent
        if (impl_->unloading)
            return Error(ErrorCode::InvalidState, "unload already in progress");
        impl_->unloading = true;
        s = std::move(impl_->session);
        jobs = std::move(impl_->jobs);
        impl_->jobs.clear();
    }
    for (const auto id : jobs) (void)impl_->queue.cancel(id);
    for (const auto id : jobs) {
        for (;;) {
            auto st = impl_->queue.status(id);
            if (!st || is_terminal(st.value().state))
                break;
            (void)impl_->queue.wait(id, std::chrono::milliseconds(100));
        }
    }
    s.reset();  // last strong reference (jobs hold weak_ptr): frees context + weights here
    {
        std::lock_guard l(impl_->mm);
        impl_->metrics = EngineMetrics{};
    }
    {
        std::lock_guard l(impl_->rm);
        impl_->results.clear();
    }
    std::lock_guard l(impl_->mu);
    impl_->unloading = false;
    return {};
}

Result<JobId> DiffusionImageEngine::generate(const ImageRequest& req) {
    return submit("diffusion.generate", req, false);
}

Result<JobId> DiffusionImageEngine::edit(const ImageRequest& req) {
    return submit("diffusion.edit", req, true);
}

Result<JobId> DiffusionImageEngine::submit(const std::string& name, const ImageRequest& req,
                                           bool require_init) {
    if (req.prompt.empty())
        return Error(ErrorCode::InvalidArgument, "prompt is empty");
    if (require_init && !req.init_image)
        return Error(ErrorCode::InvalidArgument, "edit requires an init_image");
    if (req.width < kMinDimension || req.width > kMaxDimension || req.height < kMinDimension ||
        req.height > kMaxDimension)
        return Error(ErrorCode::InvalidArgument, "width/height must be in 64..4096");
    if (req.width % 8 != 0 || req.height % 8 != 0)
        return Error(ErrorCode::InvalidArgument, "width/height must be multiples of 8");
    if (!valid_preset_name(req.preset))
        return Error(ErrorCode::InvalidArgument, "preset must be empty, 'fast' or 'quality'");
    if (req.preset.empty() && (req.steps == 0 || req.steps > kMaxSteps))
        return Error(ErrorCode::InvalidArgument, "steps must be in 1..150");
    if (!(req.strength >= 0.0f && req.strength <= 1.0f))
        return Error(ErrorCode::InvalidArgument, "strength must be in 0..1");

    std::lock_guard l(impl_->mu);
    if (!impl_->session || impl_->unloading)
        return Error(ErrorCode::InvalidState, "no model loaded");
    impl_->jobs.erase(std::remove_if(impl_->jobs.begin(), impl_->jobs.end(),
                                     [&](JobId id) {
                                         auto st = impl_->queue.status(id);
                                         return !st || is_terminal(st.value().state);
                                     }),
                      impl_->jobs.end());

    std::weak_ptr<Session> weak = impl_->session;
    Impl* impl = impl_.get();
    auto id = impl_->queue.submit(
        name,
        [weak, impl, req](JobContext& jc) -> Result<void> {
            auto s = weak.lock();
            if (!s)
                return Error(ErrorCode::Cancelled, "model was unloaded before the job started");
            ProgressSink sink = [&jc](double frac, std::string msg) { jc.progress(frac, std::move(msg)); };
            auto r = impl->render(s->ctx, s->family, req, jc.token(), sink);
            if (!r)
                return r.error();
            std::lock_guard l(impl->rm);
            impl->results[jc.id()] = std::move(r.value());
            return {};
        },
        [](const JobEvent&) {});
    if (!id)
        return id.error();
    impl_->jobs.push_back(id.value());
    return id.value();
}

Result<ImageResult> DiffusionImageEngine::image_result(JobId id) const {
    std::lock_guard l(impl_->rm);
    const auto it = impl_->results.find(id);
    if (it == impl_->results.end())
        return Error(ErrorCode::NotFound, "no result recorded for this job");
    return it->second;
}

Result<ImageResult> DiffusionImageEngine::generate_image(const ImageRequest& req,
                                                         const CancellationToken& token) {
    return generate_image(req, token, ProgressSink{});
}

Result<ImageResult> DiffusionImageEngine::edit_image(const ImageRequest& req,
                                                     const CancellationToken& token) {
    return edit_image(req, token, ProgressSink{});
}

Result<ImageResult> DiffusionImageEngine::generate_image(const ImageRequest& req,
                                                         const CancellationToken& token,
                                                         const ProgressSink& sink) {
    return run(req, token, sink, false);
}

Result<ImageResult> DiffusionImageEngine::edit_image(const ImageRequest& req, const CancellationToken& token,
                                                     const ProgressSink& sink) {
    return run(req, token, sink, true);
}

Result<ImageResult> DiffusionImageEngine::run(const ImageRequest& req, const CancellationToken& token,
                                              const ProgressSink& sink, bool require_init) {
    if (req.prompt.empty())
        return Error(ErrorCode::InvalidArgument, "prompt is empty");
    if (require_init && !req.init_image)
        return Error(ErrorCode::InvalidArgument, "edit requires an init_image");
    if (req.width < kMinDimension || req.width > kMaxDimension || req.height < kMinDimension ||
        req.height > kMaxDimension)
        return Error(ErrorCode::InvalidArgument, "width/height must be in 64..4096");
    if (req.width % 8 != 0 || req.height % 8 != 0)
        return Error(ErrorCode::InvalidArgument, "width/height must be multiples of 8");
    if (!valid_preset_name(req.preset))
        return Error(ErrorCode::InvalidArgument, "preset must be empty, 'fast' or 'quality'");
    if (req.preset.empty() && (req.steps == 0 || req.steps > kMaxSteps))
        return Error(ErrorCode::InvalidArgument, "steps must be in 1..150");
    if (!(req.strength >= 0.0f && req.strength <= 1.0f))
        return Error(ErrorCode::InvalidArgument, "strength must be in 0..1");
    if (!(req.cfg_scale >= 0.0f && req.cfg_scale <= 30.0f))
        return Error(ErrorCode::InvalidArgument, "cfg_scale must be in 0..30");

    std::shared_ptr<Session> s;
    {
        std::lock_guard l(impl_->mu);
        if (!impl_->session)
            return Error(ErrorCode::InvalidState, "no model loaded");
        if (impl_->unloading)
            return Error(ErrorCode::InvalidState, "unload is in progress");
        s = impl_->session;
    }
    if (token.cancelled())
        return Error(ErrorCode::Cancelled, "cancelled before generation started");
    std::lock_guard run_lock(s->run_mu);
    try {
        auto r = impl_->render(s->ctx, s->family, req, token, sink);
        if (r) {
            std::lock_guard l(impl_->mm);
            impl_->metrics.image_generation_ms = r.value().generation_ms;
            impl_->metrics.image_steps = r.value().steps;
        }
        return r;
    } catch (const std::exception& ex) {
        const std::string what = ex.what();
        const std::string low = lower(what);
        if (low.find("outofmemory") != std::string::npos || low.find("out of memory") != std::string::npos ||
            low.find("bad_alloc") != std::string::npos)
            return Error(ErrorCode::OutOfMemory, std::string("image generation ran out of memory: ") + what);
        if (low.find("devicelost") != std::string::npos || low.find("device lost") != std::string::npos)
            return Error(ErrorCode::BackendError,
                         std::string("Vulkan device lost during generation: ") + what);
        return Error(ErrorCode::Internal, std::string("image generation failed: ") + what);
    }
}

Result<ImageResult> DiffusionImageEngine::Impl::render(sd_ctx_t* ctx, const std::string& family,
                                                       const ImageRequest& request,
                                                       const CancellationToken& token,
                                                       const ProgressSink& sink) {
    const auto t0 = Clock::now();
    // A preset replaces steps and cfg with the measured point for the loaded family. A preset without a
    // table entry for this family is rejected rather than silently ignored.
    ImageRequest req = request;
    if (!req.preset.empty()) {
        const auto pt = preset_for(family, req.preset);
        if (!pt)
            return Error(ErrorCode::InvalidArgument,
                         "preset '" + req.preset + "' is not defined for the loaded model" +
                             (family.empty() ? std::string(" (family not recognised; pass explicit steps)")
                                             : " family '" + family + "'"));
        req.steps = pt->steps;
        req.cfg_scale = pt->cfg;
    } else if (family == "flux3" && req.cfg_scale == 0.0f) {
        // FLUX.3 Klein is guidance-distilled: the engine-wide default cfg over-saturates it and produces
        // artefacts (docs/test-evidence/wave-f/try-vk-4.png), so an unset cfg means 1.0 for this family.
        req.cfg_scale = 1.0f;
    }
    if (req.steps == 0 || req.steps > kMaxSteps)
        return Error(ErrorCode::InvalidArgument, "steps must be in 1..150");
    sd_img_gen_opt_t* opt = sd_img_gen_opt_create();
    if (!opt)
        return Error(ErrorCode::OutOfMemory, "could not allocate image options");
    struct OptGuard {
        sd_img_gen_opt_t* p;
        ~OptGuard() { sd_opt_free(p); }
    } opt_guard{opt};

    auto set_int = [&](const char* key, std::int64_t value) -> Result<void> {
        const sd_status_t st = sd_opt_set_int(opt, key, value);
        if (st != SD_STATUS_OK)
            return map_status(st, std::string("could not set '") + key + "'");
        return {};
    };
    auto set_float = [&](const char* key, double value) -> Result<void> {
        const sd_status_t st = sd_opt_set_float(opt, key, value);
        if (st != SD_STATUS_OK)
            return map_status(st, std::string("could not set '") + key + "'");
        return {};
    };
    auto set_string = [&](const char* key, const std::string& value) -> Result<void> {
        const sd_status_t st = sd_opt_set_string(opt, key, value.c_str());
        if (st != SD_STATUS_OK)
            return map_status(st, std::string("could not set '") + key + "'");
        return {};
    };

    Result<void> cfg = set_string("prompt", req.prompt);
    if (cfg && !req.negative_prompt.empty())
        cfg = set_string("negative_prompt", req.negative_prompt);
    if (cfg)
        cfg = set_int("width", req.width);
    if (cfg)
        cfg = set_int("height", req.height);
    if (cfg)
        cfg = set_int("sample.steps", req.steps);
    if (cfg)
        cfg = set_int("batch_count", 1);
    if (cfg)
        cfg = set_int("seed", req.seed ? static_cast<std::int64_t>(*req.seed) : -1);
    if (cfg && req.cfg_scale > 0.0f)
        cfg = set_float("sample.guidance.txt_cfg", req.cfg_scale);

    sd_tensor_t* init_tensor = nullptr;
    if (req.init_image) {
        auto decoded = decode_input(*req.init_image);
        if (!decoded)
            return decoded.error();
        const std::int64_t shape[3] = {3, decoded.value().width, decoded.value().height};
        init_tensor =
            sd_tensor_create(SD_TENSOR_U8, 3, shape, decoded.value().rgb.data(), decoded.value().rgb.size());
        if (!init_tensor)
            return last_error(ErrorCode::Internal, "could not create the init image tensor");
        if (cfg) {
            const sd_status_t st = sd_opt_set_tensor(opt, "init_image", init_tensor);
            if (st != SD_STATUS_OK)
                cfg = map_status(st, "could not set 'init_image'");
        }
        if (cfg)
            cfg = set_float("strength", req.strength);
    }
    if (init_tensor)
        sd_tensor_free(init_tensor);  // opt retains the tensor on success
    if (!cfg)
        return cfg.error();

    ProgressState pstate;
    pstate.token = &token;
    pstate.ctx = ctx;
    pstate.sink = &sink;
    sd_set_generation_progress_callback(ctx, progress_cb, &pstate);

    sd_result_t* result = nullptr;
    const std::uint64_t log_mark = LogSink::instance().mark();
    const sd_status_t gen = sd_generate_image(ctx, opt, &result);
    sd_set_generation_progress_callback(ctx, nullptr, nullptr);

    if (token.cancelled()) {
        if (result)
            sd_result_free(result);
        return Error(ErrorCode::Cancelled, "image generation was cancelled");
    }
    if (gen != SD_STATUS_OK || !result) {
        const std::string log = LogSink::instance().since(log_mark);
        const std::string why = tail_error_lines(log);
        if (result)
            sd_result_free(result);
        return map_status(
            gen, "stable-diffusion.cpp image generation failed" + (why.empty() ? std::string() : ": " + why));
    }
    struct ResultGuard {
        sd_result_t* p;
        ~ResultGuard() { sd_result_free(p); }
    } result_guard{result};

    if (sd_result_get_image_count(result) == 0)
        return Error(ErrorCode::BackendError, "the engine returned no image");
    const sd_tensor_t* img = sd_result_get_image(result, 0);
    if (!img)
        return Error(ErrorCode::BackendError, "the engine returned a null image tensor");
    if (sd_tensor_type(img) != SD_TENSOR_U8 || sd_tensor_rank(img) != 3)
        return Error(ErrorCode::BackendError, "unexpected image tensor layout");
    const std::int64_t channels = sd_tensor_dim(img, 0);
    const std::int64_t width = sd_tensor_dim(img, 1);
    const std::int64_t height = sd_tensor_dim(img, 2);
    if ((channels != 3 && channels != 4) || width <= 0 || height <= 0 || width > INT32_MAX ||
        height > INT32_MAX)
        return Error(ErrorCode::BackendError, "unexpected image tensor dimensions");
    const std::size_t expected = static_cast<std::size_t>(channels) * static_cast<std::size_t>(width) *
                                 static_cast<std::size_t>(height);
    if (sd_tensor_byte_size(img) != expected)
        return Error(ErrorCode::BackendError, "image tensor size mismatch");
    const auto* pixels = static_cast<const std::uint8_t*>(sd_tensor_data(img));
    if (!pixels)
        return Error(ErrorCode::BackendError, "image tensor has no data");

    ImageResult out;
    out.width = static_cast<std::uint32_t>(width);
    out.height = static_cast<std::uint32_t>(height);
    out.steps = req.steps;
    out.preset = req.preset;
    out.format = "png";
    if (!stbi_write_png_to_func(png_sink, &out.bytes, static_cast<int>(width), static_cast<int>(height),
                                static_cast<int>(channels), pixels, static_cast<int>(width * channels)))
        return Error(ErrorCode::Internal, "PNG encoding failed");
    out.generation_ms = ms_since(t0);
    return out;
}

}  // namespace hcs::engines
