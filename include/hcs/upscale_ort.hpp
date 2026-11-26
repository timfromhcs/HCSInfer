#pragma once
// ONNX Runtime backend of IUpscaleEngine, loaded DYNAMICALLY at run time (LoadLibrary of onnxruntime.dll).
//
// There is no compile-time ONNX Runtime dependency: the build neither needs ORT headers nor libraries, and a
// binary without ORT still runs (the engine just reports Unsupported). The ORT C API is a table of function
// pointers returned by OrtGetApiBase()->GetApi(version); this module reads the entries it needs by their fixed
// slot numbers (the table order is append-only across ORT releases) and verifies the table at load time with a
// status round-trip. The first 100 slots were additionally exercised end to end against a real
// onnxruntime.dll with a synthetic ONNX model (tests/unit/upscale, HCS_TEST_ORT_DIR / HCS_TEST_ONNX_MODEL).
//
// Where onnxruntime.dll may come from: only the directories the caller passes (the control plane passes the
// approved model roots' `upscale/` folders and the folder of the executable). Nothing is searched on PATH.
// Execution provider: "auto" prefers DirectML when the loaded runtime exports it and a session can be created,
// otherwise CPU; the provider that is actually used is reported in info().provider (a fallback is never silent).
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "hcs/upscale.hpp"

namespace hcs::upscale {

struct OrtConfig {
    std::vector<std::filesystem::path> runtime_dirs;  // directories (already approved) holding onnxruntime.dll
    std::filesystem::path model_path;                 // .onnx file (already resolved)
    std::string provider = "auto";                    // "auto" | "dml" | "cpu"
    int threads = 0;                                  // 0 => ORT default
};

// First directory in `dirs` that contains onnxruntime.dll (absolute path of the DLL), or empty.
std::filesystem::path find_ort_runtime(const std::vector<std::filesystem::path>& dirs);

// First *.onnx file in `dir` (sorted by name) or empty. Used to pick the model in <root>/upscale/.
std::filesystem::path find_upscale_model(const std::filesystem::path& dir);

class OrtUpscaleEngine final : public IUpscaleEngine {
public:
    explicit OrtUpscaleEngine(OrtConfig cfg);
    ~OrtUpscaleEngine() override;
    OrtUpscaleEngine(const OrtUpscaleEngine&) = delete;
    OrtUpscaleEngine& operator=(const OrtUpscaleEngine&) = delete;

    // Loads the runtime and the model on first use (cached, including a failure). Errors:
    //   Unsupported  onnxruntime.dll or the model file is missing, or this platform has no backend
    //   ModelError   the model cannot be loaded / has an unsupported signature (non-float, non-NCHW, ...)
    //   BackendError the runtime rejected the call
    Result<void> ready() const override;
    ModelInfo info() const override;
    Result<void> run_tile(const float* in, std::uint32_t w, std::uint32_t h, float* out,
                          const CancellationToken& token) override;
    // "1.31.1" etc. once the runtime is loaded, otherwise "".
    std::string runtime_version() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace hcs::upscale
