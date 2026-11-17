// llama.cpp text engine adapter. Only this translation unit (and its siblings in this directory)
// may include llama.h / ggml headers.
#include "hcs/engines/llama_engine.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <random>
#include <regex>
#include <thread>

#include "ggml-backend.h"
#include "llama.h"
#include "mtmd-helper.h"
#include "mtmd.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace hcs::engines {

namespace {

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// ---------------------------------------------------------------------------------------------
// Process-wide capture of llama.cpp/ggml log output (backend + device evidence, error context).
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
        constexpr std::size_t kMax = 256 * 1024;
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
    LogSink() { echo_ = std::getenv("HCS_LLAMA_VERBOSE") != nullptr; }
    mutable std::mutex m_;
    std::string buf_;
    std::uint64_t base_ = 0;
    bool echo_ = false;
};

void log_cb(ggml_log_level, const char* text, void*) {
    LogSink::instance().append(text);
}

void ensure_backend_init() {
    static std::once_flag once;
    std::call_once(once, [] {
        llama_log_set(log_cb, nullptr);
        mtmd_helper_log_set(log_cb, nullptr);  // libmtmd has its own log hook (also covers mtmd_log_set)
        // Intentionally never paired with llama_backend_free(): tearing down ggml/Vulkan global state
        // during static destruction is unsafe; the OS reclaims it at process exit.
        llama_backend_init();
    });
}

bool is_gpu(ggml_backend_dev_t d) {
    const auto t = ggml_backend_dev_type(d);
    return t == GGML_BACKEND_DEVICE_TYPE_GPU || t == GGML_BACKEND_DEVICE_TYPE_IGPU;
}

int physical_cores() {
#ifdef _WIN32
    DWORD len = 0;
    GetLogicalProcessorInformation(nullptr, &len);
    if (len > 0) {
        std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(len /
                                                               sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
        if (GetLogicalProcessorInformation(info.data(), &len)) {
            int n = 0;
            for (const auto& i : info) {
                if (i.Relationship == RelationProcessorCore)
                    ++n;
            }
            if (n > 0)
                return n;
        }
    }
#endif
    const unsigned hw = std::thread::hardware_concurrency();
    return hw >= 2 ? static_cast<int>(hw / 2) : 1;
}

std::size_t complete_utf8_prefix(const std::string& s) {
    const std::size_t n = s.size();
    for (std::size_t k = 1; k <= std::min<std::size_t>(3, n); ++k) {
        const unsigned char c = static_cast<unsigned char>(s[n - k]);
        if ((c & 0xC0) == 0x80)
            continue;  // continuation byte
        const std::size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        return need > k ? n - k : n;
    }
    return n;
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
        if (low.find("error") != std::string::npos || low.find("failed") != std::string::npos ||
            low.find("unknown model architecture") != std::string::npos ||
            low.find("unsupported") != std::string::npos || low.find("mismatch") != std::string::npos ||
            low.find("hint:") != std::string::npos)
            keep.push_back(line);
    }
    std::string out;
    const std::size_t from = keep.size() > 3 ? keep.size() - 3 : 0;
    for (std::size_t i = from; i < keep.size(); ++i) out += (out.empty() ? "" : " | ") + keep[i];
    if (out.size() > 500)
        out.resize(500);
    return out;
}

ErrorCode classify(const std::string& log, ErrorCode fallback) {
    const std::string low = lower(log);
    if (low.find("out of memory") != std::string::npos ||
        low.find("outofdevicememory") != std::string::npos ||
        low.find("outofhostmemory") != std::string::npos ||
        low.find("failed to allocate") != std::string::npos)
        return ErrorCode::OutOfMemory;
    if (low.find("unknown model architecture") != std::string::npos ||
        low.find("unsupported") != std::string::npos ||
        low.find("cannot be used as main model") != std::string::npos)
        return ErrorCode::Unsupported;
    return fallback;
}

Result<llama_load_mode> parse_load_mode(const std::string& m, bool& set) {
    set = true;
    if (m.empty()) {
        set = false;
        return LLAMA_LOAD_MODE_AUTO;
    }
    if (m == "auto")
        return LLAMA_LOAD_MODE_AUTO;
    if (m == "none")
        return LLAMA_LOAD_MODE_NONE;
    if (m == "mmap")
        return LLAMA_LOAD_MODE_MMAP;
    if (m == "mlock")
        return LLAMA_LOAD_MODE_MLOCK;
    if (m == "mmap+mlock")
        return LLAMA_LOAD_MODE_MMAP_MLOCK;
    if (m == "direct_io")
        return LLAMA_LOAD_MODE_DIRECT_IO;
    return Error(ErrorCode::InvalidArgument,
                 "unknown load_mode '" + m + "' (auto|none|mmap|mlock|mmap+mlock|direct_io)");
}

// Everything that must live and die together. Freed in dependency order (context, then model).
struct Session {
    llama_model* model = nullptr;
    llama_context* ctx = nullptr;
    const llama_vocab* vocab = nullptr;
    mtmd_context* mctx = nullptr;  // vision projector (null => text-only session)
    std::string chat_template;     // empty => none
    std::uint32_t n_ctx = 0;
    std::mutex run_mu;  // one owner of the context
    ~Session() {
        if (mctx)
            mtmd_free(mctx);  // before the model it was initialised from
        if (ctx)
            llama_free(ctx);
        if (model)
            llama_model_free(model);
    }
};

// RAII for libmtmd objects (every path out of the vision job releases them).
struct BitmapDeleter {
    void operator()(mtmd_bitmap* b) const {
        if (b)
            mtmd_bitmap_free(b);
    }
};
struct ChunksDeleter {
    void operator()(mtmd_input_chunks* c) const {
        if (c)
            mtmd_input_chunks_free(c);
    }
};
using BitmapPtr = std::unique_ptr<mtmd_bitmap, BitmapDeleter>;
using ChunksPtr = std::unique_ptr<mtmd_input_chunks, ChunksDeleter>;

struct AbortCtx {
    CancellationToken token;
};
bool abort_cb(void* data) {
    return static_cast<AbortCtx*>(data)->token.cancelled();
}

}  // namespace

// ---------------------------------------------------------------------------------------------
struct LlamaTextEngine::Impl {
    explicit Impl(JobQueue& queue) : q(queue) {}

    JobQueue& q;
    mutable std::mutex mu;  // session, job list, loading flag
    std::shared_ptr<Session> session;
    std::vector<JobId> jobs;
    bool loading = false;
    bool unloading = false;

    mutable std::mutex mm;  // metrics + load log
    EngineMetrics metrics;
    std::string load_log;

    void set_metrics(const std::function<void(EngineMetrics&)>& f) {
        std::lock_guard l(mm);
        f(metrics);
    }

    Result<void> run_generation(const std::shared_ptr<Session>& s, const TextRequest& req, JobContext& jc,
                                const std::vector<std::uint8_t>* image = nullptr);
    Result<JobId> submit(const char* name, const TextRequest& req,
                         std::shared_ptr<const std::vector<std::uint8_t>> image, EventCallback on_event);
};

LlamaTextEngine::LlamaTextEngine(JobQueue& queue) : impl_(std::make_unique<Impl>(queue)) {}
LlamaTextEngine::~LlamaTextEngine() {
    (void)unload();
}

bool LlamaTextEngine::vulkan_compiled() {
#ifdef HCS_LLAMA_VULKAN
    return true;
#else
    return false;
#endif
}

std::vector<GpuDeviceInfo> LlamaTextEngine::gpu_devices() {
    ensure_backend_init();
    std::vector<GpuDeviceInfo> out;
    for (std::size_t i = 0; i < ggml_backend_dev_count(); ++i) {
        auto* d = ggml_backend_dev_get(i);
        if (!is_gpu(d))
            continue;
        GpuDeviceInfo g;
        g.name = ggml_backend_dev_name(d);
        g.description = ggml_backend_dev_description(d);
        std::size_t free_b = 0, total_b = 0;
        ggml_backend_dev_memory(d, &free_b, &total_b);
        g.memory_free_bytes = free_b;
        g.memory_total_bytes = total_b;
        out.push_back(std::move(g));
    }
    return out;
}

std::string LlamaTextEngine::upstream_build_info() {
    ensure_backend_init();
    const char* s = llama_print_system_info();
    return s ? s : "unknown";
}

bool LlamaTextEngine::loaded() const {
    std::lock_guard l(impl_->mu);
    return impl_->session != nullptr;
}

EngineMetrics LlamaTextEngine::metrics() const {
    std::lock_guard l(impl_->mm);
    return impl_->metrics;
}

std::string LlamaTextEngine::last_load_log() const {
    std::lock_guard l(impl_->mm);
    return impl_->load_log;
}

Result<void> LlamaTextEngine::load(const LoadRequest& req) {
    try {
        return load_impl(req);
    } catch (const std::exception& ex) {
        const std::string low = lower(ex.what());
        const bool oom = low.find("outofhostmemory") != std::string::npos ||
                         low.find("outofdevicememory") != std::string::npos ||
                         low.find("out of memory") != std::string::npos ||
                         low.find("bad_alloc") != std::string::npos;
        return Error(oom ? ErrorCode::OutOfMemory : ErrorCode::BackendError,
                     std::string("exception while loading model: ") + ex.what());
    }
}

Result<void> LlamaTextEngine::load_impl(const LoadRequest& req) {
    ensure_backend_init();
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

    // ---- validation (no allocation, no upstream calls) ----
    if (req.model_path.empty())
        return Error(ErrorCode::InvalidArgument, "model_path is empty");
    if (req.context_tokens == 0 || req.context_tokens > (1u << 20))
        return Error(ErrorCode::InvalidArgument, "context_tokens must be in 1..1048576");
    if (!req.projector_path.empty()) {
        std::ifstream f(req.projector_path, std::ios::binary);
        if (!f)
            return Error(ErrorCode::NotFound, "cannot open projector (mmproj) file: " + req.projector_path);
        char magic[4] = {};
        f.read(magic, 4);
        if (f.gcount() != 4 || std::string(magic, 4) != "GGUF")
            return Error(ErrorCode::ModelError,
                         "projector is not a GGUF file (bad magic): " + req.projector_path);
    }
    bool lm_set = false;
    auto lm = parse_load_mode(req.load_mode, lm_set);
    if (!lm)
        return lm.error();
    {
        std::ifstream f(req.model_path, std::ios::binary);
        if (!f)
            return Error(ErrorCode::NotFound, "cannot open model file: " + req.model_path);
        char magic[4] = {};
        f.read(magic, 4);
        if (f.gcount() != 4 || std::string(magic, 4) != "GGUF")
            return Error(ErrorCode::ModelError, "not a GGUF file (bad magic): " + req.model_path);
    }

    // ---- backend selection ----
    ggml_backend_dev_t gpu = nullptr;
    for (std::size_t i = 0; i < ggml_backend_dev_count(); ++i) {
        auto* d = ggml_backend_dev_get(i);
        if (!is_gpu(d))
            continue;
        if (!req.device.empty()) {
            const std::string n = ggml_backend_dev_name(d);
            const std::string desc = ggml_backend_dev_description(d);
            if (n != req.device && desc.find(req.device) == std::string::npos)
                continue;
        }
        gpu = d;
        break;
    }
    bool use_gpu = false;
    switch (req.backend) {
        case Backend::Cpu:
            break;
        case Backend::Vulkan:
            if (!gpu) {
                if (!vulkan_compiled())
                    return Error(
                        ErrorCode::Unsupported,
                        "Vulkan requested but this build has no Vulkan backend (HCS_ENABLE_VULKAN=OFF)");
                return Error(ErrorCode::BackendError,
                             "Vulkan requested but no GPU device is available" +
                                 (req.device.empty() ? std::string() : " matching '" + req.device + "'"));
            }
            use_gpu = true;
            break;
        case Backend::Auto:
            use_gpu = gpu != nullptr;
            break;
    }

    // ---- load model (rolled back by Session destructor on any failure path) ----
    const std::uint64_t log_mark = LogSink::instance().mark();
    const auto t0 = Clock::now();
    auto s = std::make_shared<Session>();

    llama_model_params mp = llama_model_default_params();
    ggml_backend_dev_t devices[2] = {nullptr, nullptr};
    if (use_gpu) {
        devices[0] = gpu;
        mp.n_gpu_layers = req.gpu_layers < 0 ? 999 : req.gpu_layers;
    } else {
        mp.n_gpu_layers = 0;  // devices = {nullptr}: empty device list => CPU only
    }
    mp.devices = devices;
    if (lm_set)
        mp.load_mode = lm.value();

    s->model = llama_model_load_from_file(req.model_path.c_str(), mp);
    if (!s->model) {
        const std::string log = LogSink::instance().since(log_mark);
        const std::string why = tail_error_lines(log);
        {
            std::lock_guard l(impl_->mm);
            impl_->load_log = log;
        }
        return Error(classify(log, ErrorCode::ModelError),
                     "llama.cpp could not load the model" + (why.empty() ? std::string() : ": " + why));
    }
    s->vocab = llama_model_get_vocab(s->model);

    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = req.context_tokens;
    cp.n_batch = std::min<std::uint32_t>(req.context_tokens, 2048);
    // Vision models with non-causal image attention need n_ubatch >= image tokens per chunk.
    cp.n_ubatch = std::min<std::uint32_t>(cp.n_batch, req.projector_path.empty() ? 512 : 2048);
    cp.n_seq_max = 1;
    const int threads = req.threads > 0 ? req.threads : physical_cores();
    cp.n_threads = threads;
    cp.n_threads_batch = threads;
    cp.offload_kqv = use_gpu;
    cp.no_perf = true;
    s->ctx = llama_init_from_model(s->model, cp);
    if (!s->ctx) {
        const std::string log = LogSink::instance().since(log_mark);
        const std::string why = tail_error_lines(log);
        {
            std::lock_guard l(impl_->mm);
            impl_->load_log = log;
        }
        return Error(classify(log, ErrorCode::BackendError),
                     "failed to create llama context (ctx=" + std::to_string(req.context_tokens) + ")" +
                         (why.empty() ? std::string() : ": " + why));
    }
    s->n_ctx = llama_n_ctx(s->ctx);

    if (!req.projector_path.empty()) {
        mtmd_context_params mcp = mtmd_context_params_default();
        mcp.use_gpu = use_gpu;
        if (use_gpu)
            mcp.device = gpu;
        mcp.n_threads = threads;
        mcp.print_timings = false;
        mcp.warmup = false;
        s->mctx = mtmd_init_from_file(req.projector_path.c_str(), s->model, mcp);
        if (!s->mctx) {
            const std::string log = LogSink::instance().since(log_mark);
            const std::string why = tail_error_lines(log);
            {
                std::lock_guard l(impl_->mm);
                impl_->load_log = log;
            }
            return Error(
                classify(log, ErrorCode::ModelError),
                "could not initialise the vision projector (mmproj does not match this model, or the "
                "projector architecture is unsupported)" +
                    (why.empty() ? std::string() : ": " + why));
        }
        if (!mtmd_support_vision(s->mctx))
            return Error(ErrorCode::Unsupported,
                         "the projector does not provide image (vision) input (audio-only projector?)");
    }
    if (const char* t = llama_model_chat_template(s->model, nullptr))
        s->chat_template = t;

    const std::string log = LogSink::instance().since(log_mark);
    EngineMetrics m;
    m.backend = use_gpu ? "vulkan" : "cpu";
    m.device = use_gpu
                   ? std::string(ggml_backend_dev_description(gpu)) + " (" + ggml_backend_dev_name(gpu) + ")"
                   : "CPU";
    m.load_ms = ms_since(t0);
    m.resident_bytes = llama_model_size(s->model);
    m.total_layers = llama_model_n_layer(s->model);
    {
        std::smatch mt;
        if (std::regex_search(log, mt, std::regex(R"(offloaded (\d+)/(\d+) layers to GPU)")))
            m.offloaded_layers = std::stoi(mt[1].str());
        else
            m.offloaded_layers = use_gpu ? -1 : 0;
    }
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

Result<void> LlamaTextEngine::unload() {
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
    for (const auto id : jobs) (void)impl_->q.cancel(id);
    for (const auto id : jobs) {
        for (;;) {
            auto st = impl_->q.status(id);
            if (!st || is_terminal(st.value().state))
                break;  // NotFound => evicted => finished
            (void)impl_->q.wait(id, std::chrono::milliseconds(100));
        }
    }
    s.reset();  // last strong reference (jobs only hold weak_ptr): frees context + model here
    {
        std::lock_guard l(impl_->mm);
        impl_->metrics = EngineMetrics{};
    }
    std::lock_guard l(impl_->mu);
    impl_->unloading = false;
    return {};
}

bool LlamaTextEngine::vision_loaded() const {
    std::lock_guard l(impl_->mu);
    return impl_->session && impl_->session->mctx;
}

Result<JobId> LlamaTextEngine::analyze_image(const VisionRequest& req) {
    return analyze_image(req, EventCallback{});
}

Result<JobId> LlamaTextEngine::analyze_image(const VisionRequest& req, EventCallback on_event) {
    if (req.image.bytes.empty())
        return Error(ErrorCode::InvalidArgument, "image is empty");
    if (req.image.bytes.size() > kMaxVisionImageBytes)
        return Error(ErrorCode::InvalidArgument,
                     "image exceeds the " + std::to_string(kMaxVisionImageBytes >> 20) + " MiB limit");
    if (req.text.prompt.empty())
        return Error(ErrorCode::InvalidArgument, "prompt is empty");
    if (req.text.prompt.find(mtmd_default_marker()) != std::string::npos ||
        req.text.system_prompt.find(mtmd_default_marker()) != std::string::npos)
        return Error(ErrorCode::InvalidArgument, "the prompt must not contain the multimodal media marker");
    {
        std::lock_guard l(impl_->mu);
        if (!impl_->session || impl_->unloading)
            return Error(ErrorCode::InvalidState, "no model loaded");
        if (!impl_->session->mctx)
            return Error(ErrorCode::Unsupported,
                         "the loaded model has no vision projector: load it with a matching mmproj file "
                         "(LoadRequest::projector_path)");
    }
    return impl_->submit("llama.vision", req.text,
                         std::make_shared<const std::vector<std::uint8_t>>(req.image.bytes),
                         std::move(on_event));
}

Result<JobId> LlamaTextEngine::generate(const TextRequest& req) {
    return generate(req, EventCallback{});
}

Result<JobId> LlamaTextEngine::generate(const TextRequest& req, EventCallback on_event) {
    return impl_->submit("llama.generate", req, nullptr, std::move(on_event));
}

Result<JobId> LlamaTextEngine::Impl::submit(const char* name, const TextRequest& req,
                                            std::shared_ptr<const std::vector<std::uint8_t>> image,
                                            EventCallback on_event) {
    if (req.prompt.empty())
        return Error(ErrorCode::InvalidArgument, "prompt is empty");
    if (req.max_tokens == 0 || req.max_tokens > (1u << 20))
        return Error(ErrorCode::InvalidArgument, "max_tokens must be in 1..1048576");
    if (!(req.temperature >= 0.0f && req.temperature <= 10.0f))
        return Error(ErrorCode::InvalidArgument, "temperature must be in 0..10");

    std::lock_guard l(mu);
    if (!session || unloading)
        return Error(ErrorCode::InvalidState, "no model loaded");

    // drop finished job ids so the list stays bounded
    jobs.erase(std::remove_if(jobs.begin(), jobs.end(),
                              [&](JobId id) {
                                  auto st = q.status(id);
                                  return !st || is_terminal(st.value().state);
                              }),
               jobs.end());

    std::weak_ptr<Session> weak = session;
    Impl* impl = this;
    auto id = q.submit(
        name,
        [weak, impl, copy = req, image = std::move(image)](JobContext& jc) -> Result<void> {
            auto s = weak.lock();
            if (!s)
                return Error(ErrorCode::Cancelled, "model was unloaded before the job started");
            std::lock_guard run(s->run_mu);
            try {
                return impl->run_generation(s, copy, jc, image.get());
            } catch (const std::exception& ex) {
                // ggml-vulkan reports driver failures (OOM, device lost) as vk:: exceptions from
                // llama_process.
                const std::string what = ex.what();
                const std::string low = lower(what);
                if (low.find("outofhostmemory") != std::string::npos ||
                    low.find("outofdevicememory") != std::string::npos ||
                    low.find("out of memory") != std::string::npos)
                    return Error(ErrorCode::OutOfMemory, "backend out of memory during generation: " + what);
                if (low.find("devicelost") != std::string::npos)
                    return Error(ErrorCode::BackendError,
                                 "GPU device lost during generation; unload and reload the model: " + what);
                return Error(ErrorCode::BackendError, "backend exception during generation: " + what);
            }
        },
        std::move(on_event));
    if (!id)
        return id.error();
    jobs.push_back(id.value());
    return id.value();
}

Result<void> LlamaTextEngine::Impl::run_generation(const std::shared_ptr<Session>& s, const TextRequest& req,
                                                   JobContext& jc, const std::vector<std::uint8_t>* image) {
    if (jc.cancelled())
        return Error(ErrorCode::Cancelled, "cancelled before start");
    const auto t_start = Clock::now();

    // ---- prompt: chat template then tokenize ----
    // Vision: the user turn carries the media marker; mtmd_tokenize() swaps it for the image chunk.
    const std::string user_text = image ? std::string(mtmd_default_marker()) + "\n" + req.prompt : req.prompt;
    std::string text;
    if (req.apply_chat_template && !s->chat_template.empty()) {
        std::vector<llama_chat_message> msgs;
        if (!req.system_prompt.empty())
            msgs.push_back({"system", req.system_prompt.c_str()});
        msgs.push_back({"user", user_text.c_str()});
        std::size_t total = 0;
        for (const auto& m : msgs) total += std::strlen(m.content);
        std::vector<char> buf(std::max<std::size_t>(2 * total + 256, 1024));
        int n = llama_chat_apply_template(s->chat_template.c_str(), msgs.data(), msgs.size(), true,
                                          buf.data(), static_cast<int32_t>(buf.size()));
        if (n > static_cast<int>(buf.size())) {
            buf.resize(static_cast<std::size_t>(n));
            n = llama_chat_apply_template(s->chat_template.c_str(), msgs.data(), msgs.size(), true,
                                          buf.data(), static_cast<int32_t>(buf.size()));
        }
        if (n < 0)
            return Error(ErrorCode::Unsupported,
                         "the model's chat template is not supported by llama_chat_apply_template; "
                         "disable apply_chat_template to send a raw prompt");
        text.assign(buf.data(), static_cast<std::size_t>(n));
    } else {
        text = (req.system_prompt.empty() ? std::string() : req.system_prompt + "\n") + user_text;
    }

    int n_tok = 0;
    std::vector<llama_token> tokens;
    ChunksPtr chunks;
    BitmapPtr bitmap;
    if (image) {
        // ---- decode + tokenize the image (headers first: pixel cap before any pixel is decoded) ----
        mtmd_helper_init_opt hopt = mtmd_helper_init_opt_default();
        mtmd_helper_media_meta meta{};
        if (mtmd_helper_get_media_meta(image->data(), image->size(), hopt, &meta) != 0 ||
            meta.type != MTMD_HELPER_MEDIA_TYPE_IMAGE)
            return Error(
                ErrorCode::InvalidArgument,
                "the input is not a supported image (png/jpeg/bmp/gif) or its dimensions are out of range");
        if (meta.width == 0 || meta.height == 0 ||
            static_cast<std::uint64_t>(meta.width) * meta.height > kMaxVisionImagePixels)
            return Error(ErrorCode::InvalidArgument, "image dimensions " + std::to_string(meta.width) + "x" +
                                                         std::to_string(meta.height) + " exceed the " +
                                                         std::to_string(kMaxVisionImagePixels >> 20) +
                                                         " Mpixel limit");
        mtmd_helper_bitmap_wrapper bw =
            mtmd_helper_bitmap_init_from_buf(s->mctx, image->data(), image->size(), false, hopt);
        if (bw.video_ctx)
            mtmd_helper_video_free(bw.video_ctx);
        bitmap.reset(bw.bitmap);
        if (!bitmap || mtmd_bitmap_is_audio(bitmap.get()))
            return Error(ErrorCode::InvalidArgument,
                         "could not decode the image (corrupt or unsupported format)");
        if (jc.cancelled())
            return Error(ErrorCode::Cancelled, "cancelled before start");
        chunks.reset(mtmd_input_chunks_init());
        mtmd_input_text it{text.c_str(), text.size(), true, true};
        const mtmd_bitmap* bms[1] = {bitmap.get()};
        const int trc = mtmd_tokenize(s->mctx, chunks.get(), &it, bms, 1);
        if (trc != 0)
            return Error(trc == 2 ? ErrorCode::ModelError : ErrorCode::InvalidArgument,
                         trc == 2 ? "image preprocessing failed for this projector"
                                  : "multimodal tokenization failed (media marker / bitmap count mismatch)");
        n_tok = static_cast<int>(mtmd_helper_get_n_tokens(chunks.get()));
        if (n_tok <= 0)
            return Error(ErrorCode::InvalidArgument, "prompt tokenization produced no tokens");
    } else {
        n_tok = -llama_tokenize(s->vocab, text.c_str(), static_cast<int32_t>(text.size()), nullptr, 0, true,
                                true);
        if (n_tok <= 0)
            return Error(ErrorCode::InvalidArgument, "prompt tokenization produced no tokens");
        tokens.resize(static_cast<std::size_t>(n_tok));
        if (llama_tokenize(s->vocab, text.c_str(), static_cast<int32_t>(text.size()), tokens.data(), n_tok,
                           true, true) < 0)
            return Error(ErrorCode::Internal, "tokenization failed");
    }
    // M-RoPE models consume fewer positions than tokens; the context-fit check uses the larger (tokens).
    if (static_cast<std::uint32_t>(n_tok) >= s->n_ctx)
        return Error(ErrorCode::InvalidArgument,
                     "prompt (" + std::to_string(n_tok) + " tokens) does not fit the context (" +
                         std::to_string(s->n_ctx) + "); load with a larger context or shorten the prompt");

    // ---- sampler / batch / abort hook (all released by Cleanup) ----
    llama_sampler* smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
    if (req.temperature <= 0.0f) {
        llama_sampler_chain_add(smpl, llama_sampler_init_greedy());
    } else {
        const std::uint32_t seed = req.seed ? static_cast<std::uint32_t>(*req.seed) : std::random_device{}();
        llama_sampler_chain_add(smpl, llama_sampler_init_top_k(40));
        llama_sampler_chain_add(smpl, llama_sampler_init_top_p(0.95f, 1));
        llama_sampler_chain_add(smpl, llama_sampler_init_temp(req.temperature));
        llama_sampler_chain_add(smpl, llama_sampler_init_dist(seed));
    }
    llama_batch_ext* batch = llama_batch_ext_init(s->ctx);
    AbortCtx abort_ctx{jc.token()};
    llama_set_abort_callback(s->ctx, abort_cb, &abort_ctx);
    struct Cleanup {
        llama_sampler* smpl;
        llama_batch_ext* batch;
        llama_context* ctx;
        ~Cleanup() {
            llama_set_abort_callback(ctx, nullptr, nullptr);
            if (batch)
                llama_batch_ext_free(batch);
            if (smpl)
                llama_sampler_free(smpl);
        }
    } cleanup{smpl, batch, s->ctx};
    if (!batch)
        return Error(ErrorCode::BackendError, "llama_batch_ext_init failed");

    llama_memory_clear(llama_get_memory(s->ctx), true);  // single-turn request: start from an empty KV cache

    auto fill = [&](const llama_token* t, int n, llama_pos pos0) -> bool {
        llama_batch_ext_clear(batch);
        for (int i = 0; i < n; ++i) {
            const int idx = llama_batch_ext_add_token(batch, 0, t[i]);
            if (idx < 0)
                return false;
            const llama_pos pos = pos0 + i;
            llama_batch_ext_set_pos(batch, idx, &pos);
        }
        llama_batch_ext_set_output_logits(batch, n - 1, true);
        return true;
    };
    auto decode_error = [&](int rc) -> Error {
        if (jc.cancelled() || rc == 2)
            return Error(ErrorCode::Cancelled, "generation cancelled");
        if (rc == 1)
            return Error(ErrorCode::OutOfMemory, "no KV cache slot for the batch (context too small)");
        return Error(ErrorCode::BackendError, "llama_process failed with code " + std::to_string(rc));
    };

    // ---- prompt evaluation in n_batch chunks ----
    const int chunk = static_cast<int>(std::max<std::uint32_t>(1, llama_n_batch(s->ctx)));
    const auto t_prompt0 = Clock::now();
    llama_pos start_pos = n_tok;
    if (image) {
        // Text chunks are decoded, image chunks encoded by the projector then decoded as embeddings.
        llama_pos new_past = 0;
        const int rc = mtmd_helper_eval_chunks(s->mctx, s->ctx, chunks.get(), 0, 0, chunk, true, &new_past);
        if (rc != 0) {
            if (jc.cancelled())
                return Error(ErrorCode::Cancelled, "generation cancelled");
            return Error(ErrorCode::BackendError, "vision prompt evaluation failed (code " +
                                                      std::to_string(rc) +
                                                      "); image encode/decode error, possibly out of memory");
        }
        start_pos = new_past;
    } else {
        for (int off = 0; off < n_tok; off += chunk) {
            if (jc.cancelled())
                return Error(ErrorCode::Cancelled, "generation cancelled");
            const int n = std::min(chunk, n_tok - off);
            if (!fill(tokens.data() + off, n, off))
                return Error(ErrorCode::Internal, "batch fill failed");
            const int rc = llama_process(s->ctx, LLAMA_PROCESS_TYPE_DECODE, batch);
            if (rc != 0)
                return decode_error(rc);
        }
    }
    const double prompt_ms = ms_since(t_prompt0);

    // ---- generation ----
    std::string pending;  // bytes of an incomplete UTF-8 sequence
    int n_gen = 0;
    int pos = static_cast<int>(start_pos);
    double ttft_ms = 0.0;
    Clock::time_point t_first{};
    for (;;) {
        if (jc.cancelled())
            return Error(ErrorCode::Cancelled, "generation cancelled");
        llama_token tok = llama_sampler_sample(smpl, s->ctx, -1);
        if (llama_vocab_is_eog(s->vocab, tok))
            break;
        std::string piece(256, '\0');
        int n = llama_token_to_piece(s->vocab, tok, piece.data(), static_cast<int>(piece.size()), 0, true);
        if (n < 0) {
            piece.assign(static_cast<std::size_t>(-n), '\0');
            n = llama_token_to_piece(s->vocab, tok, piece.data(), static_cast<int>(piece.size()), 0, true);
        }
        if (n < 0)
            return Error(ErrorCode::Internal, "token_to_piece failed");
        piece.resize(static_cast<std::size_t>(n));
        pending += piece;
        const std::size_t ok_len = complete_utf8_prefix(pending);
        if (ok_len > 0) {
            jc.output(pending.substr(0, ok_len));
            pending.erase(0, ok_len);
        }
        if (++n_gen == 1) {
            t_first = Clock::now();
            ttft_ms = std::chrono::duration<double, std::milli>(t_first - t_start).count();
        }
        if (static_cast<std::uint32_t>(n_gen) >= req.max_tokens)
            break;
        if (static_cast<std::uint32_t>(pos) + 1 >= s->n_ctx)
            break;  // context full
        if (!fill(&tok, 1, pos))
            return Error(ErrorCode::Internal, "batch fill failed");
        ++pos;
        const int rc = llama_process(s->ctx, LLAMA_PROCESS_TYPE_DECODE, batch);
        if (rc != 0)
            return decode_error(rc);
    }
    if (!pending.empty())
        jc.output(pending);

    const double decode_ms = n_gen > 1 ? ms_since(t_first) : 0.0;
    set_metrics([&](EngineMetrics& m) {
        m.ttft_ms = ttft_ms;
        m.prompt_tokens = static_cast<std::uint64_t>(n_tok);
        m.prompt_tokens_per_second = prompt_ms > 0.0 ? n_tok * 1000.0 / prompt_ms : 0.0;
        m.decode_tokens = static_cast<std::uint64_t>(n_gen > 0 ? n_gen - 1 : 0);
        m.decode_tokens_per_second = decode_ms > 0.0 ? (n_gen - 1) * 1000.0 / decode_ms : 0.0;
        m.tokens_per_second = m.decode_tokens_per_second;
    });
    return {};
}

}  // namespace hcs::engines
