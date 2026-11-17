// `HCSInfer vision` - image understanding through the llama engine adapter (libmtmd inside).
#include "vision_cmd.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>
#include <ostream>

#include "hcs/fs_util.hpp"
#include "hcs/model_registry.hpp"
#include "hcs/result.hpp"

#ifdef HCS_HAVE_LLAMA
#include "hcs/engines/llama_engine.hpp"
#include "hcs/job_queue.hpp"
#endif

namespace hcs::cli {

namespace fs = std::filesystem;
using nlohmann::json;

const char* vision_help() {
    return "Usage: HCSInfer vision --model <path-or-id> --image <file> --prompt \"...\" [options]\n"
           "\n"
           "Analyses one image with a multimodal GGUF model (llama.cpp + libmtmd). The model needs its "
           "vision\n"
           "projector (mmproj GGUF): pass --mmproj, or place exactly one mmproj*.gguf next to the model "
           "file.\n"
           "Supported image formats: png, jpeg, bmp, gif. Limits: 32 MiB encoded, 16 Mpixel decoded.\n"
           "\n"
           "Options:\n"
           "  --model <path-or-id>   Multimodal text-model GGUF path or model id (required)\n"
           "  --mmproj <path>        Projector GGUF (default: the single mmproj*.gguf beside the model)\n"
           "  --image <file>         Image file (required)\n"
           "  --prompt <text>        Question / instruction about the image (required)\n"
           "  --system <text>        System prompt\n"
           "  --backend <b>          auto (default) | cpu | vulkan (no silent fallback when explicit)\n"
           "  --device <name>        GPU device name/description substring\n"
           "  --gpu-layers <n>       Layers to offload (default: all when using a GPU)\n"
           "  --ctx <n>              Context tokens (default 4096)\n"
           "  --max-tokens <n>       Max generated tokens (default 256)\n"
           "  --temp <t>             Temperature; 0 = greedy (default 0.2)\n"
           "  --seed <n>             Sampling seed\n"
           "  --threads <n>          CPU threads (default: physical cores)\n"
           "  --json                 Machine-readable result (text + metrics) on stdout\n"
           "\n"
           "Ctrl-C cancels the running generation (exit code 6).\n";
}

#ifndef HCS_HAVE_LLAMA

int run_vision(std::vector<std::string> args, const std::string&, std::ostream& out, std::ostream& err) {
    for (const auto& a : args) {
        if (a == "--help" || a == "-h") {
            out << vision_help();
            return 0;
        }
    }
    err << "error: this build has no llama engine (external/llama.cpp missing) [unsupported]\n";
    return exit_code(ErrorCode::Unsupported);
}

#else

namespace {

std::atomic<bool> g_interrupted{false};
void vision_on_sigint(int) {
    g_interrupted.store(true);
}

int fail(std::ostream& err, const Error& e) {
    err << "error: " << e.message << " [" << to_string(e.code) << "]\n";
    return e.exit_code();
}
int fail(std::ostream& err, ErrorCode c, const std::string& m) {
    return fail(err, Error(c, m));
}

bool take_value(std::vector<std::string>& a, const std::string& flag, std::string& value, bool& bad) {
    auto it = std::find(a.begin(), a.end(), flag);
    if (it == a.end())
        return false;
    if (it + 1 == a.end()) {
        bad = true;
        return true;
    }
    value = *(it + 1);
    a.erase(it, it + 2);
    return true;
}
bool take_flag(std::vector<std::string>& a, const std::string& flag) {
    auto it = std::find(a.begin(), a.end(), flag);
    if (it == a.end())
        return false;
    a.erase(it);
    return true;
}

fs::path state_dir(const std::string& override_dir) {
    if (!override_dir.empty())
        return fs::path(override_dir);
    if (const char* e = std::getenv("HCS_STATE_DIR"); e && *e)
        return fs::path(e);
    if (const char* l = std::getenv("LOCALAPPDATA"); l && *l)
        return fs::path(l) / "HCSInfer";
    if (const char* h = std::getenv("HOME"); h && *h)
        return fs::path(h) / ".hcsinfer";
    return fs::path(".hcsinfer");
}

Result<std::string> resolve_model(const std::string& spec, const std::string& state_override) {
    std::error_code ec;
    if (spec.find("..") != std::string::npos && fs::exists(spec, ec))
        return Error(ErrorCode::SecurityViolation, "model path must not contain '..'");
    if (fs::is_regular_file(spec, ec))
        return fs::weakly_canonical(spec, ec).string();
    const fs::path reg_file = state_dir(state_override) / "registry.json";
    if (fs::exists(reg_file, ec)) {
        auto text = read_file_limited(reg_file, 64u << 20);
        if (!text)
            return text.error();
        ModelRegistry reg;
        if (auto r = reg.load_json(text.value()); !r)
            return r.error();
        if (auto m = reg.get(spec); m)
            return m.value().path;
    }
    return Error(ErrorCode::NotFound,
                 "model '" + spec + "' is neither an existing file nor a registered model id");
}

// The single mmproj*.gguf beside the model (case-insensitive prefix); ambiguity is an error, not a guess.
Result<std::string> find_sibling_projector(const fs::path& model) {
    std::error_code ec;
    std::vector<fs::path> found;
    for (const auto& e : fs::directory_iterator(model.parent_path(), ec)) {
        std::error_code ec2;
        if (!e.is_regular_file(ec2))
            continue;
        std::string n = e.path().filename().string();
        std::transform(n.begin(), n.end(), n.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (n.rfind("mmproj", 0) == 0 && e.path().extension() == ".gguf")
            found.push_back(e.path());
    }
    if (found.empty())
        return Error(
            ErrorCode::InvalidArgument,
            "no vision projector given: pass --mmproj <file> (no mmproj*.gguf found beside the model)");
    if (found.size() > 1)
        return Error(ErrorCode::InvalidArgument,
                     "several mmproj*.gguf files sit beside the model; pass --mmproj <file> explicitly");
    return found.front().string();
}

}  // namespace

int run_vision(std::vector<std::string> args, const std::string& state_override, std::ostream& out,
               std::ostream& err) {
    if (std::any_of(args.begin(), args.end(),
                    [](const std::string& a) { return a == "--help" || a == "-h"; })) {
        out << vision_help();
        return 0;
    }
    std::string model, mmproj, image, prompt, system, backend = "auto", device;
    std::string v_gpu, v_ctx, v_max, v_temp, v_seed, v_thr;
    bool bad = false;
    take_value(args, "--model", model, bad);
    take_value(args, "--mmproj", mmproj, bad);
    take_value(args, "--image", image, bad);
    const bool has_prompt = take_value(args, "--prompt", prompt, bad);
    take_value(args, "--system", system, bad);
    take_value(args, "--backend", backend, bad);
    take_value(args, "--device", device, bad);
    take_value(args, "--gpu-layers", v_gpu, bad);
    take_value(args, "--ctx", v_ctx, bad);
    take_value(args, "--max-tokens", v_max, bad);
    take_value(args, "--temp", v_temp, bad);
    take_value(args, "--seed", v_seed, bad);
    take_value(args, "--threads", v_thr, bad);
    const bool as_json = take_flag(args, "--json");
    if (bad)
        return fail(err, ErrorCode::InvalidArgument, "an option is missing its value");
    if (!args.empty())
        return fail(err, ErrorCode::InvalidArgument, "unexpected argument: " + args.front());
    if (model.empty())
        return fail(err, ErrorCode::InvalidArgument, "--model is required");
    if (image.empty())
        return fail(err, ErrorCode::InvalidArgument, "--image is required");
    if (!has_prompt || prompt.empty())
        return fail(err, ErrorCode::InvalidArgument, "--prompt is required");

    LoadRequest lr;
    VisionRequest vr;
    vr.text.temperature = 0.2f;
    try {
        if (!v_gpu.empty())
            lr.gpu_layers = std::stoi(v_gpu);
        if (!v_ctx.empty())
            lr.context_tokens = static_cast<std::uint32_t>(std::stoul(v_ctx));
        if (!v_thr.empty())
            lr.threads = std::stoi(v_thr);
        if (!v_max.empty())
            vr.text.max_tokens = static_cast<std::uint32_t>(std::stoul(v_max));
        if (!v_temp.empty())
            vr.text.temperature = std::stof(v_temp);
        if (!v_seed.empty())
            vr.text.seed = std::stoull(v_seed);
    } catch (const std::exception&) {
        return fail(err, ErrorCode::InvalidArgument, "a numeric option has an invalid value");
    }
    if (backend == "cpu")
        lr.backend = Backend::Cpu;
    else if (backend == "vulkan")
        lr.backend = Backend::Vulkan;
    else if (backend == "auto")
        lr.backend = Backend::Auto;
    else
        return fail(err, ErrorCode::InvalidArgument, "--backend must be auto, cpu or vulkan");
    lr.device = device;
    vr.text.prompt = prompt;
    vr.text.system_prompt = system;

    // Image file: regular file, no '..', size-bounded read.
    {
        std::error_code ec;
        if (image.find("..") != std::string::npos)
            return fail(err, ErrorCode::SecurityViolation, "image path must not contain '..'");
        if (!fs::is_regular_file(image, ec))
            return fail(err, ErrorCode::NotFound, "image file not found: " + image);
        auto bytes = read_file_limited(image, engines::LlamaTextEngine::kMaxVisionImageBytes);
        if (!bytes)
            return fail(err, bytes.error());
        vr.image.bytes.assign(bytes.value().begin(), bytes.value().end());
    }

    auto path = resolve_model(model, state_override);
    if (!path)
        return fail(err, path.error());
    lr.model_path = path.value();
    lr.model_id = model;
    if (mmproj.empty()) {
        auto p = find_sibling_projector(fs::path(lr.model_path));
        if (!p)
            return fail(err, p.error());
        lr.projector_path = p.value();
    } else {
        std::error_code ec;
        if (mmproj.find("..") != std::string::npos)
            return fail(err, ErrorCode::SecurityViolation, "mmproj path must not contain '..'");
        if (!fs::is_regular_file(mmproj, ec))
            return fail(err, ErrorCode::NotFound, "mmproj file not found: " + mmproj);
        lr.projector_path = fs::weakly_canonical(mmproj, ec).string();
    }

    JobQueue queue;
    engines::LlamaTextEngine engine(queue);
    if (auto r = engine.load(lr); !r)
        return fail(err, r.error());
    const EngineMetrics lm = engine.metrics();
    if (!as_json) {
        err << "[hcsinfer] backend=" << lm.backend << " device=" << lm.device
            << " load_ms=" << static_cast<long>(lm.load_ms)
            << " vision=" << (engine.vision_loaded() ? "yes" : "no") << "\n";
    }

    std::signal(SIGINT, vision_on_sigint);
    std::mutex text_mu;
    std::string text;
    auto id = engine.analyze_image(vr, [&](const JobEvent& ev) {
        if (ev.kind != JobEventKind::Output)
            return;
        std::lock_guard l(text_mu);
        text += ev.text;
        if (!as_json) {
            out << ev.text;
            out.flush();
        }
    });
    if (!id) {
        const int rc = fail(err, id.error());
        (void)engine.unload();
        return rc;
    }
    bool cancelled_by_signal = false;
    while (!queue.wait(id.value(), std::chrono::milliseconds(50))) {
        if (g_interrupted.load() && !cancelled_by_signal) {
            cancelled_by_signal = true;
            (void)queue.cancel(id.value());
        }
    }
    auto st = queue.status(id.value());
    if (!st) {
        const int rc = fail(err, st.error());
        (void)engine.unload();
        return rc;
    }
    const EngineMetrics m = engine.metrics();
    if (!as_json)
        out << "\n";
    int rc = 0;
    if (!as_json)
        err << "[hcsinfer] ttft_ms=" << static_cast<long>(m.ttft_ms) << " prompt_tokens=" << m.prompt_tokens
            << " decode_tps=" << m.decode_tokens_per_second << "\n";
    if (st.value().state == JobState::Failed) {
        rc = fail(err, *st.value().error);
    } else if (st.value().state == JobState::Cancelled) {
        err << "cancelled\n";
        rc = exit_code(ErrorCode::Cancelled);
    }
    if (as_json) {
        json j;
        j["schema_version"] = 1;
        j["model"] = model;
        j["backend"] = lm.backend;
        j["device"] = lm.device;
        j["image_bytes"] = vr.image.bytes.size();
        j["prompt_chars"] = prompt.size();  // prompt content is never echoed or logged
        j["text"] = text;
        j["state"] = std::string(to_string(st.value().state));
        if (st.value().error)
            j["error"] = {{"code", std::string(to_string(st.value().error->code))},
                          {"message", st.value().error->message}};
        j["metrics"] = {{"load_ms", lm.load_ms},
                        {"ttft_ms", m.ttft_ms},
                        {"prompt_tokens", m.prompt_tokens},
                        {"prompt_tokens_per_second", m.prompt_tokens_per_second},
                        {"decode_tokens", m.decode_tokens},
                        {"decode_tokens_per_second", m.decode_tokens_per_second}};
        out << j.dump(2) << "\n";
    }
    (void)engine.unload();
    return rc;
}

#endif  // HCS_HAVE_LLAMA

}  // namespace hcs::cli
