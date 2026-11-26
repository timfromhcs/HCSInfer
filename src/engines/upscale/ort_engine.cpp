// Dynamically loaded ONNX Runtime backend (see hcs/upscale_ort.hpp). No ORT header is included: the C API is
// used through its function-pointer table with locally declared signatures.
#include <algorithm>
#include <cstring>
#include <optional>

#include "hcs/upscale_ort.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace hcs::upscale {

namespace fs = std::filesystem;

fs::path find_ort_runtime(const std::vector<fs::path>& dirs) {
    for (const auto& d : dirs) {
        std::error_code ec;
        const fs::path p = d / "onnxruntime.dll";
        if (fs::is_regular_file(p, ec))
            return fs::absolute(p, ec);
    }
    return {};
}

fs::path executable_dir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
        return fs::path(buf).parent_path();
#endif
    return {};
}

fs::path find_upscale_model(const fs::path& dir) {
    std::error_code ec;
    std::vector<fs::path> found;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec))
            continue;
        std::string ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".onnx")
            found.push_back(e.path());
    }
    if (found.empty())
        return {};
    std::sort(found.begin(), found.end());
    return found.front();
}

#ifdef _WIN32

namespace {

// OrtApi slot numbers (append-only table; see the header comment for how they were verified).
enum Slot : int {
    CreateStatus = 0,
    GetErrorCode = 1,
    GetErrorMessage = 2,
    CreateEnv = 3,
    CreateSession = 7,
    Run = 9,
    CreateSessionOptions = 10,
    SetSessionExecutionMode = 13,
    DisableMemPattern = 17,
    SetSessionGraphOptimizationLevel = 23,
    SetIntraOpNumThreads = 24,
    SessionGetInputCount = 30,
    SessionGetOutputCount = 31,
    SessionGetInputTypeInfo = 33,
    SessionGetInputName = 36,
    SessionGetOutputName = 37,
    CreateTensorWithDataAsOrtValue = 49,
    GetTensorMutableData = 51,
    CastTypeInfoToTensorInfo = 55,
    GetTensorElementType = 60,
    GetDimensionsCount = 61,
    GetDimensions = 62,
    GetTensorTypeAndShape = 65,
    CreateCpuMemoryInfo = 69,
    AllocatorFree = 76,
    GetAllocatorWithDefaultOptions = 78,
    ReleaseEnv = 92,
    ReleaseStatus = 93,
    ReleaseMemoryInfo = 94,
    ReleaseSession = 95,
    ReleaseValue = 96,
    ReleaseTypeInfo = 98,
    ReleaseTensorTypeAndShapeInfo = 99,
    ReleaseSessionOptions = 100,
};
constexpr std::uint32_t kApiVersion = 14;  // oldest table that has every slot above; newer DLLs serve it too
constexpr int kElemFloat = 1;

using St = void*;  // OrtStatus*
struct OrtApiBase {
    const void* const* (*GetApi)(std::uint32_t);
    const char* (*GetVersionString)();
};

std::string narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back(c < 128 ? static_cast<char>(c) : '?');
    return s;
}

}  // namespace

struct OrtUpscaleEngine::Impl {
    OrtConfig cfg;
    mutable std::mutex m;
    mutable bool tried = false;
    mutable std::optional<Error> failure;

    HMODULE dll = nullptr;
    const void* const* api = nullptr;
    std::string version;
    void* env = nullptr;
    void* opts = nullptr;
    void* sess = nullptr;
    void* meminfo = nullptr;
    std::string in_name, out_name;
    ModelInfo mi;

    template <class F>
    F fn(int slot) const {
        return reinterpret_cast<F>(const_cast<void*>(api[slot]));
    }

    std::string status_text(St st) const {
        std::string msg = "unknown ONNX Runtime error";
        int code = 0;
        if (st) {
            code = fn<int (*)(const void*)>(GetErrorCode)(st);
            if (const char* m2 = fn<const char* (*)(const void*)>(GetErrorMessage)(st))
                msg = m2;
            fn<void (*)(void*)>(ReleaseStatus)(st);
        }
        return msg + " (ort code " + std::to_string(code) + ")";
    }

    void release_all() {
        if (api) {
            if (meminfo)
                fn<void (*)(void*)>(ReleaseMemoryInfo)(meminfo);
            if (sess)
                fn<void (*)(void*)>(ReleaseSession)(sess);
            if (opts)
                fn<void (*)(void*)>(ReleaseSessionOptions)(opts);
            if (env)
                fn<void (*)(void*)>(ReleaseEnv)(env);
        }
        meminfo = sess = opts = env = nullptr;
        // The DLL stays loaded for the life of the process: unloading ONNX Runtime is not supported.
    }

    Result<void> new_session(bool dml, std::string& dml_note) {
        if (opts) {
            fn<void (*)(void*)>(ReleaseSessionOptions)(opts);
            opts = nullptr;
        }
        if (St st = fn<St (*)(void**)>(CreateSessionOptions)(&opts))
            return Error(ErrorCode::BackendError, "CreateSessionOptions: " + status_text(st));
        if (St st = fn<St (*)(void*, int)>(SetSessionGraphOptimizationLevel)(opts, 99))
            return Error(ErrorCode::BackendError, "graph optimization level: " + status_text(st));
        if (cfg.threads > 0)
            if (St st = fn<St (*)(void*, int)>(SetIntraOpNumThreads)(opts, cfg.threads))
                return Error(ErrorCode::BackendError, "intra-op threads: " + status_text(st));
        if (dml) {
            auto append = reinterpret_cast<St (*)(void*, int)>(
                GetProcAddress(dll, "OrtSessionOptionsAppendExecutionProvider_DML"));
            if (!append) {
                dml_note = "this onnxruntime.dll does not export the DirectML provider";
                return Error(ErrorCode::Unsupported, dml_note);
            }
            // DirectML requires sequential execution and no memory-pattern optimisation.
            if (St st = fn<St (*)(void*)>(DisableMemPattern)(opts))
                return Error(ErrorCode::BackendError, "DisableMemPattern: " + status_text(st));
            if (St st = fn<St (*)(void*, int)>(SetSessionExecutionMode)(opts, 0))
                return Error(ErrorCode::BackendError, "SetSessionExecutionMode: " + status_text(st));
            if (St st = append(opts, 0)) {
                dml_note = status_text(st);
                return Error(ErrorCode::Unsupported, "DirectML provider: " + dml_note);
            }
        }
        if (sess) {
            fn<void (*)(void*)>(ReleaseSession)(sess);
            sess = nullptr;
        }
        if (St st = fn<St (*)(const void*, const wchar_t*, const void*, void**)>(CreateSession)(
                env, cfg.model_path.c_str(), opts, &sess)) {
            const std::string t = status_text(st);
            if (dml)
                dml_note = t;
            return Error(ErrorCode::ModelError,
                         "cannot load '" + cfg.model_path.filename().string() + "': " + t);
        }
        return {};
    }

    Result<void> load() {
        const fs::path dllp = find_ort_runtime(cfg.runtime_dirs);
        if (dllp.empty()) {
            std::string dirs;
            for (const auto& d : cfg.runtime_dirs) dirs += (dirs.empty() ? "" : ", ") + d.string();
            return Error(
                ErrorCode::Unsupported,
                "onnxruntime.dll was not found" + (dirs.empty() ? std::string() : " in: " + dirs) +
                    ". Place a trusted ONNX Runtime (CPU or DirectML build) next to HCSInfer.exe or in "
                    "<model root>/upscale/");
        }
        std::error_code ec;
        if (cfg.model_path.empty() || !fs::is_regular_file(cfg.model_path, ec))
            return Error(ErrorCode::Unsupported,
                         "no upscale model: put a .onnx super-resolution model (e.g. Real-ESRGAN x4) in "
                         "<model root>/upscale/");
        dll = LoadLibraryExW(dllp.c_str(), nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!dll)
            return Error(ErrorCode::Unsupported, "LoadLibrary failed for '" + dllp.string() + "' (error " +
                                                     std::to_string(GetLastError()) + ")");
        auto base_fn = reinterpret_cast<const OrtApiBase* (*)()>(GetProcAddress(dll, "OrtGetApiBase"));
        const OrtApiBase* base = base_fn ? base_fn() : nullptr;
        if (!base)
            return Error(ErrorCode::Unsupported, "'" + dllp.string() + "' does not export OrtGetApiBase");
        if (const char* v = base->GetVersionString ? base->GetVersionString() : nullptr)
            version = v;
        api = base->GetApi(kApiVersion);
        if (!api)
            return Error(ErrorCode::Unsupported, "onnxruntime " + version +
                                                     " does not provide C API version " +
                                                     std::to_string(kApiVersion) + " (too old)");
        // Table self-check: a status created by slot 0 must be readable through slots 1 and 2.
        {
            St st = fn<St (*)(int, const char*)>(CreateStatus)(2, "hcs-selfcheck");
            const bool good =
                st && fn<int (*)(const void*)>(GetErrorCode)(st) == 2 &&
                std::strcmp(fn<const char* (*)(const void*)>(GetErrorMessage)(st), "hcs-selfcheck") == 0;
            if (st)
                fn<void (*)(void*)>(ReleaseStatus)(st);
            if (!good)
                return Error(ErrorCode::Unsupported,
                             "ONNX Runtime " + version + ": API table self-check failed");
        }
        if (St st = fn<St (*)(int, const char*, void**)>(CreateEnv)(3, "hcs-upscale", &env))
            return Error(ErrorCode::BackendError, "CreateEnv: " + status_text(st));
        if (St st = fn<St (*)(int, int, void**)>(CreateCpuMemoryInfo)(1, 0, &meminfo))
            return Error(ErrorCode::BackendError, "CreateCpuMemoryInfo: " + status_text(st));

        std::string provider = "CPU";
        std::string dml_note;
        const bool want_dml = cfg.provider != "cpu";
        Result<void> sr = Error(ErrorCode::Internal, "unreachable");
        bool dml_ok = false;
        if (want_dml) {
            sr = new_session(true, dml_note);
            dml_ok = sr.ok();
            if (!dml_ok && cfg.provider == "dml")
                return sr.error();  // explicit request: no silent fallback
        }
        if (!dml_ok) {
            sr = new_session(false, dml_note);
            if (!sr)
                return sr.error();
            if (want_dml)
                provider = "CPU (DirectML unavailable: " + dml_note + ")";
        } else {
            provider = "DirectML";
        }
        mi.provider = provider;
        mi.name = cfg.model_path.filename().string();

        // Signature: one float NCHW input, one float NCHW output.
        void* alloc = nullptr;
        if (St st = fn<St (*)(void**)>(GetAllocatorWithDefaultOptions)(&alloc))
            return Error(ErrorCode::BackendError, "GetAllocatorWithDefaultOptions: " + status_text(st));
        std::size_t n_in = 0, n_out = 0;
        if (St st = fn<St (*)(const void*, std::size_t*)>(SessionGetInputCount)(sess, &n_in))
            return Error(ErrorCode::BackendError, "SessionGetInputCount: " + status_text(st));
        if (St st = fn<St (*)(const void*, std::size_t*)>(SessionGetOutputCount)(sess, &n_out))
            return Error(ErrorCode::BackendError, "SessionGetOutputCount: " + status_text(st));
        if (n_in != 1 || n_out != 1)
            return Error(ErrorCode::ModelError,
                         "the upscale model must have exactly one input and one output (has " +
                             std::to_string(n_in) + "/" + std::to_string(n_out) + ")");
        for (int which = 0; which < 2; ++which) {
            char* name = nullptr;
            St st = which == 0 ? fn<St (*)(const void*, std::size_t, void*, char**)>(SessionGetInputName)(
                                     sess, 0, alloc, &name)
                               : fn<St (*)(const void*, std::size_t, void*, char**)>(SessionGetOutputName)(
                                     sess, 0, alloc, &name);
            if (st)
                return Error(ErrorCode::BackendError, "reading tensor names: " + status_text(st));
            (which == 0 ? in_name : out_name) = name ? name : "";
            if (name)
                (void)fn<St (*)(void*, void*)>(AllocatorFree)(alloc, name);
        }
        void* ti = nullptr;
        if (St st = fn<St (*)(const void*, std::size_t, void**)>(SessionGetInputTypeInfo)(sess, 0, &ti))
            return Error(ErrorCode::BackendError, "SessionGetInputTypeInfo: " + status_text(st));
        const void* tinfo = nullptr;
        std::int64_t dims[8] = {};
        std::size_t nd = 0;
        int et = 0;
        St st1 = fn<St (*)(const void*, const void**)>(CastTypeInfoToTensorInfo)(ti, &tinfo);
        St st2 = st1 ? nullptr : fn<St (*)(const void*, int*)>(GetTensorElementType)(tinfo, &et);
        St st3 =
            (st1 || st2) ? nullptr : fn<St (*)(const void*, std::size_t*)>(GetDimensionsCount)(tinfo, &nd);
        St st4 = (st1 || st2 || st3 || nd != 4)
                     ? nullptr
                     : fn<St (*)(const void*, std::int64_t*, std::size_t)>(GetDimensions)(tinfo, dims, nd);
        const bool bad = st1 || st2 || st3 || st4;
        const std::string why = st1   ? status_text(st1)
                                : st2 ? status_text(st2)
                                : st3 ? status_text(st3)
                                : st4 ? status_text(st4)
                                      : "";
        fn<void (*)(void*)>(ReleaseTypeInfo)(ti);
        if (bad)
            return Error(ErrorCode::BackendError, "reading the model input signature: " + why);
        if (et != kElemFloat)
            return Error(ErrorCode::ModelError, "the upscale model input must be float32 (element type " +
                                                    std::to_string(et) + "); fp16 models are not supported");
        if (nd != 4)
            return Error(ErrorCode::ModelError,
                         "the upscale model input must be NCHW (rank 4), got rank " + std::to_string(nd));
        if (dims[1] > 0 && dims[1] != 3)
            return Error(ErrorCode::ModelError, "the upscale model must take 3 colour channels");
        if (dims[2] > 0 || dims[3] > 0) {
            if (dims[2] != dims[3] || dims[2] <= 0)
                return Error(ErrorCode::ModelError, "fixed-size upscale models must have a square input");
            mi.fixed_tile = static_cast<std::uint32_t>(dims[2]);
        }
        // Probe run: detects the real scale and proves the model executes on the chosen provider.
        const std::uint32_t pw = mi.fixed_tile ? mi.fixed_tile : 16;
        mi.scale = 0;
        std::vector<float> in(static_cast<std::size_t>(pw) * pw * 3, 0.5f);
        std::size_t out_n = 0;
        auto rr = run_raw(in.data(), pw, pw, nullptr, &out_n);
        if (!rr)
            return rr.error();
        const std::size_t per = static_cast<std::size_t>(pw) * pw * 3;
        if (out_n == 0 || out_n % per != 0)
            return Error(ErrorCode::ModelError, "cannot derive the model scale from the probe output");
        int s = 1;
        while (static_cast<std::size_t>(s) * s * per < out_n) ++s;
        if (static_cast<std::size_t>(s) * s * per != out_n || s > 8)
            return Error(ErrorCode::ModelError,
                         "the model output size is not a 1..8x integer multiple of its input");
        mi.scale = s;
        return {};
    }

    // Runs one tile. out == nullptr: only reports the element count in *out_n (probe).
    Result<void> run_raw(const float* in, std::uint32_t w, std::uint32_t h, float* out, std::size_t* out_n) {
        const std::size_t plane = static_cast<std::size_t>(w) * h;
        std::vector<float> chw(plane * 3);
        for (std::size_t i = 0; i < plane; ++i)
            for (int c = 0; c < 3; ++c) chw[c * plane + i] = in[i * 3 + c];
        const std::int64_t shape[4] = {1, 3, static_cast<std::int64_t>(h), static_cast<std::int64_t>(w)};
        void* iv = nullptr;
        if (St st =
                fn<St (*)(const void*, void*, std::size_t, const std::int64_t*, std::size_t, int, void**)>(
                    CreateTensorWithDataAsOrtValue)(meminfo, chw.data(), chw.size() * sizeof(float), shape, 4,
                                                    kElemFloat, &iv))
            return Error(ErrorCode::BackendError, "CreateTensorWithDataAsOrtValue: " + status_text(st));
        struct Rel {
            Impl* o;
            void* v;
            ~Rel() {
                if (v)
                    o->fn<void (*)(void*)>(ReleaseValue)(v);
            }
        } in_rel{this, iv}, out_rel{this, nullptr};
        const char* in_names[1] = {in_name.c_str()};
        const char* out_names[1] = {out_name.c_str()};
        const void* in_vals[1] = {iv};
        void* ov = nullptr;
        if (St st = fn<St (*)(void*, const void*, const char* const*, const void* const*, std::size_t,
                              const char* const*, std::size_t, void**)>(Run)(sess, nullptr, in_names, in_vals,
                                                                             1, out_names, 1, &ov))
            return Error(ErrorCode::BackendError, "inference failed: " + status_text(st));
        out_rel.v = ov;
        void* info = nullptr;
        if (St st = fn<St (*)(const void*, void**)>(GetTensorTypeAndShape)(ov, &info))
            return Error(ErrorCode::BackendError, "GetTensorTypeAndShape: " + status_text(st));
        std::size_t nd = 0;
        std::int64_t d[8] = {};
        int et = 0;
        St s1 = fn<St (*)(const void*, std::size_t*)>(GetDimensionsCount)(info, &nd);
        St s2 = (s1 || nd != 4)
                    ? nullptr
                    : fn<St (*)(const void*, std::int64_t*, std::size_t)>(GetDimensions)(info, d, nd);
        St s3 = (s1 || s2) ? nullptr : fn<St (*)(const void*, int*)>(GetTensorElementType)(info, &et);
        const std::string why = s1 ? status_text(s1) : s2 ? status_text(s2) : s3 ? status_text(s3) : "";
        fn<void (*)(void*)>(ReleaseTensorTypeAndShapeInfo)(info);
        if (!why.empty())
            return Error(ErrorCode::BackendError, "reading the output shape: " + why);
        if (nd != 4 || et != kElemFloat || d[0] != 1 || d[1] != 3 || d[2] <= 0 || d[3] <= 0)
            return Error(ErrorCode::ModelError, "unexpected model output (need float32 [1,3,H,W])");
        const std::size_t oh = static_cast<std::size_t>(d[2]), ow = static_cast<std::size_t>(d[3]);
        if (out_n)
            *out_n = 3 * oh * ow;
        if (!out)
            return {};
        if (mi.scale > 0 &&
            (oh != static_cast<std::size_t>(h) * mi.scale || ow != static_cast<std::size_t>(w) * mi.scale))
            return Error(ErrorCode::ModelError, "model output " + std::to_string(ow) + "x" +
                                                    std::to_string(oh) + " does not match scale " +
                                                    std::to_string(mi.scale));
        void* data = nullptr;
        if (St st = fn<St (*)(void*, void**)>(GetTensorMutableData)(ov, &data))
            return Error(ErrorCode::BackendError, "GetTensorMutableData: " + status_text(st));
        const float* src = static_cast<const float*>(data);
        const std::size_t oplane = oh * ow;
        for (std::size_t i = 0; i < oplane; ++i)
            for (int c = 0; c < 3; ++c) out[i * 3 + c] = src[c * oplane + i];
        return {};
    }
};

OrtUpscaleEngine::OrtUpscaleEngine(OrtConfig cfg) : impl_(std::make_unique<Impl>()) {
    impl_->cfg = std::move(cfg);
}

OrtUpscaleEngine::~OrtUpscaleEngine() {
    std::lock_guard lk(impl_->m);
    impl_->release_all();
}

Result<void> OrtUpscaleEngine::ready() const {
    std::lock_guard lk(impl_->m);
    if (!impl_->tried) {
        impl_->tried = true;
        auto r = impl_->load();
        if (!r) {
            impl_->failure = r.error();
            impl_->release_all();
        }
    }
    if (impl_->failure)
        return *impl_->failure;
    return {};
}

ModelInfo OrtUpscaleEngine::info() const {
    std::lock_guard lk(impl_->m);
    return impl_->mi;
}

std::string OrtUpscaleEngine::runtime_version() const {
    std::lock_guard lk(impl_->m);
    return impl_->version;
}

Result<void> OrtUpscaleEngine::run_tile(const float* in, std::uint32_t w, std::uint32_t h, float* out,
                                        const CancellationToken& token) {
    if (auto r = ready(); !r)
        return r;
    if (token.cancelled())
        return Error(ErrorCode::Cancelled, "upscaling was cancelled");
    std::lock_guard lk(impl_->m);
    if (impl_->mi.fixed_tile && (w != impl_->mi.fixed_tile || h != impl_->mi.fixed_tile))
        return Error(ErrorCode::InvalidArgument, "this model only accepts fixed-size tiles");
    return impl_->run_raw(in, w, h, out, nullptr);
}

#else  // !_WIN32

struct OrtUpscaleEngine::Impl {
    OrtConfig cfg;
};
OrtUpscaleEngine::OrtUpscaleEngine(OrtConfig cfg) : impl_(std::make_unique<Impl>()) {
    impl_->cfg = std::move(cfg);
}
OrtUpscaleEngine::~OrtUpscaleEngine() = default;
Result<void> OrtUpscaleEngine::ready() const {
    return Error(ErrorCode::Unsupported, "the ONNX Runtime upscaler backend is implemented for Windows only");
}
ModelInfo OrtUpscaleEngine::info() const {
    return {};
}
std::string OrtUpscaleEngine::runtime_version() const {
    return {};
}
Result<void> OrtUpscaleEngine::run_tile(const float*, std::uint32_t, std::uint32_t, float*,
                                        const CancellationToken&) {
    return ready();
}

#endif

}  // namespace hcs::upscale
