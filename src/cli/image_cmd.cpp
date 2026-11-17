// `HCSInfer image generate|edit` - routes through the diffusion engine adapter (never raw sd_* calls).
#include "image_cmd.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <ostream>
#include <string>

#include "hcs/fs_util.hpp"
#include "hcs/model_registry.hpp"
#include "hcs/result.hpp"

#ifdef HCS_HAVE_DIFFUSION
#include "hcs/engines/diffusion_engine.hpp"
#include "hcs/job_queue.hpp"
#endif

namespace hcs::cli {

namespace fs = std::filesystem;
using nlohmann::json;

const char* image_help() {
    return "Usage: HCSInfer image <generate|edit> --model <path-or-id> --prompt \"...\" [options]\n"
           "\n"
           "Text-to-image (`generate`) and img2img/edit (`edit --input <png|jpeg>`) through the\n"
           "stable-diffusion.cpp engine adapter.\n"
           "\n"
           "Options:\n"
           "  --model <path-or-id>   Diffusion model file or a model id from `models scan` (required)\n"
           "  --prompt <text>        Positive prompt (required)\n"
           "  --negative <text>      Negative prompt\n"
           "  --input <file>         Initial image for `edit` (PNG/JPEG, <= 32 MiB, <= 16 MP)\n"
           "  --strength <0..1>      Denoising strength for `edit` (default 0.75)\n"
           "  --width <n>            Image width, 64..4096, multiple of 8 (default 512)\n"
           "  --height <n>           Image height, 64..4096, multiple of 8 (default 512)\n"
           "  --steps <n>            Sampling steps, 1..150 (default 20)\n"
           "  --preset <p>           fast | quality: measured steps/cfg point for the loaded model family\n"
           "                         (FLUX.3 Klein); overrides --steps/--cfg, rejected for other families\n"
           "  --llm <file>           Text-encoder weights for multi-file models (FLUX.3: Qwen3-VL 4B)\n"
           "  --vae <file>           VAE weights for multi-file models\n"
           "                         (without --llm/--vae a `<model>.components.json` sidecar is read)\n"
           "  --params-backend <b>   cpu (default) | disk: where module weights live between uses; disk is a\n"
           "                         capacity fallback and can be slower\n"
           "  --cfg <f>              Text CFG scale; 0 = engine/model default\n"
           "  --seed <n>             Sampling seed (default: random)\n"
           "  --backend <b>          auto (default) | cpu | vulkan; explicit failure never falls back "
           "silently\n"
           "  --device <name>        GPU device name/description substring (e.g. Vulkan0)\n"
           "  --threads <n>          CPU threads (default: engine default)\n"
           "  --load-mode <m>        auto (default) | mmap | none\n"
           "  --output <file>        Output PNG (default: hcsinfer-image-<timestamp>.png)\n"
           "  --force                Overwrite an existing output file\n"
           "  --json                 Machine-readable result on stdout\n"
           "\n"
           "Ctrl-C cancels the running generation (exit code 6).\n";
}

#ifndef HCS_HAVE_DIFFUSION

int run_image(std::vector<std::string> args, const std::string&, std::ostream& out, std::ostream& err) {
    for (const auto& a : args) {
        if (a == "--help" || a == "-h") {
            out << image_help();
            return 0;
        }
    }
    err << "error: this build has no diffusion engine (external/stable-diffusion.cpp missing) "
           "[unsupported]\n";
    return exit_code(ErrorCode::Unsupported);
}

#else

namespace {

std::atomic<bool> g_interrupted{false};
CancellationSource* g_cancel = nullptr;

extern "C" void on_image_sigint(int) {
    g_interrupted.store(true);
    if (g_cancel)
        g_cancel->request_cancel();
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

Result<ImageInput> read_input_image(const std::string& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec))
        return Error(ErrorCode::NotFound, "input image not found: " + path);
    const auto size = fs::file_size(path, ec);
    if (ec)
        return Error(ErrorCode::IoError, "cannot stat input image: " + path);
    if (size == 0)
        return Error(ErrorCode::InvalidArgument, "input image is empty");
    if (size > (32u << 20))
        return Error(ErrorCode::SecurityViolation, "input image exceeds 32 MiB");
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return Error(ErrorCode::IoError, "cannot open input image: " + path);
    ImageInput in;
    in.bytes.resize(static_cast<std::size_t>(size));
    f.read(reinterpret_cast<char*>(in.bytes.data()), static_cast<std::streamsize>(in.bytes.size()));
    if (!f)
        return Error(ErrorCode::IoError, "cannot read input image: " + path);
    return in;
}

std::string default_output() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64] = {};
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    return std::string("hcsinfer-image-") + buf + ".png";
}

}  // namespace

int run_image(std::vector<std::string> args, const std::string& state_override, std::ostream& out,
              std::ostream& err) {
    if (std::any_of(args.begin(), args.end(),
                    [](const std::string& a) { return a == "--help" || a == "-h"; })) {
        out << image_help();
        return 0;
    }
    if (args.empty()) {
        err << image_help();
        return exit_code(ErrorCode::InvalidArgument);
    }
    const std::string sub = args.front();
    args.erase(args.begin());
    if (sub != "generate" && sub != "edit")
        return fail(err, ErrorCode::InvalidArgument, "image expects 'generate' or 'edit', got '" + sub + "'");

    std::string model, prompt, negative, input, output, backend = "auto", device, load_mode;
    std::string v_width, v_height, v_steps, v_cfg, v_seed, v_strength, v_threads;
    std::string preset, llm, vae, params_backend;
    bool bad = false;
    take_value(args, "--model", model, bad);
    take_value(args, "--prompt", prompt, bad);
    take_value(args, "--negative", negative, bad);
    take_value(args, "--input", input, bad);
    take_value(args, "--output", output, bad);
    take_value(args, "--backend", backend, bad);
    take_value(args, "--device", device, bad);
    take_value(args, "--load-mode", load_mode, bad);
    take_value(args, "--preset", preset, bad);
    take_value(args, "--llm", llm, bad);
    take_value(args, "--vae", vae, bad);
    take_value(args, "--params-backend", params_backend, bad);
    take_value(args, "--width", v_width, bad);
    take_value(args, "--height", v_height, bad);
    take_value(args, "--steps", v_steps, bad);
    take_value(args, "--cfg", v_cfg, bad);
    take_value(args, "--seed", v_seed, bad);
    take_value(args, "--strength", v_strength, bad);
    take_value(args, "--threads", v_threads, bad);
    const bool as_json = take_flag(args, "--json");
    const bool force = take_flag(args, "--force");
    if (bad)
        return fail(err, ErrorCode::InvalidArgument, "an option is missing its value");
    if (!args.empty())
        return fail(err, ErrorCode::InvalidArgument, "unexpected argument: " + args.front());
    if (model.empty())
        return fail(err, ErrorCode::InvalidArgument, "--model is required");
    if (prompt.empty())
        return fail(err, ErrorCode::InvalidArgument, "--prompt is required");
    if (sub == "edit" && input.empty())
        return fail(err, ErrorCode::InvalidArgument, "image edit requires --input");

    ImageRequest req;
    req.prompt = prompt;
    req.negative_prompt = negative;
    LoadRequest lr;
    try {
        if (!v_width.empty())
            req.width = static_cast<std::uint32_t>(std::stoul(v_width));
        if (!v_height.empty())
            req.height = static_cast<std::uint32_t>(std::stoul(v_height));
        if (!v_steps.empty())
            req.steps = static_cast<std::uint32_t>(std::stoul(v_steps));
        if (!v_cfg.empty())
            req.cfg_scale = std::stof(v_cfg);
        if (!v_seed.empty())
            req.seed = std::stoull(v_seed);
        if (!v_strength.empty())
            req.strength = std::stof(v_strength);
        if (!v_threads.empty())
            lr.threads = std::stoi(v_threads);
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
    lr.load_mode = load_mode;
    lr.params_backend = params_backend;
    if (preset != "" && preset != "fast" && preset != "quality")
        return fail(err, ErrorCode::InvalidArgument, "--preset must be fast or quality");
    req.preset = preset;
    for (const std::string* comp : {&llm, &vae}) {
        if (comp->empty())
            continue;
        if (comp->find("..") != std::string::npos)
            return fail(err, ErrorCode::SecurityViolation, "component path must not contain '..'");
        std::error_code ec;
        if (!fs::is_regular_file(*comp, ec))
            return fail(err, ErrorCode::NotFound, "component file not found: " + *comp);
    }
    if (!llm.empty()) {
        std::error_code ec;
        lr.llm_path = fs::weakly_canonical(llm, ec).string();
    }
    if (!vae.empty()) {
        std::error_code ec;
        lr.vae_path = fs::weakly_canonical(vae, ec).string();
    }

    if (sub == "edit") {
        auto in = read_input_image(input);
        if (!in)
            return fail(err, in.error());
        req.init_image = std::move(in.value());
    }

    auto path = resolve_model(model, state_override);
    if (!path)
        return fail(err, path.error());
    lr.model_path = path.value();
    lr.model_id = model;

    if (output.empty())
        output = default_output();
    {
        std::error_code ec;
        if (!force && fs::exists(output, ec))
            return fail(err, ErrorCode::AlreadyExists, "output file exists (use --force): " + output);
        const fs::path parent = fs::path(output).parent_path();
        if (!parent.empty()) {
            if (!fs::is_directory(parent, ec))
                return fail(err, ErrorCode::NotFound, "output directory does not exist: " + parent.string());
        }
    }

    JobQueue queue;
    engines::DiffusionImageEngine engine(queue);
    if (auto r = engine.load(lr); !r)
        return fail(err, r.error());
    const EngineMetrics lm = engine.metrics();
    if (!as_json) {
        err << "[hcsinfer] backend=" << lm.backend << " device=" << lm.device
            << " load_ms=" << static_cast<long>(lm.load_ms) << "\n";
    }

    std::signal(SIGINT, on_image_sigint);
    CancellationSource source;
    g_cancel = &source;

    const auto t0 = std::chrono::steady_clock::now();
    auto on_progress = [&](double frac, const std::string& msg) {
        if (as_json)
            return;
        err << "\r[hcsinfer] " << static_cast<int>(frac * 100.0) << "% " << msg << "          " << std::flush;
    };
    auto result = sub == "edit" ? engine.edit_image(req, source.token(), on_progress)
                                : engine.generate_image(req, source.token(), on_progress);
    g_cancel = nullptr;
    if (!as_json)
        err << "\n";
    if (!result) {
        if (result.error().code == ErrorCode::Cancelled || g_interrupted.load()) {
            err << "cancelled\n";
            return exit_code(ErrorCode::Cancelled);
        }
        return fail(err, result.error());
    }

    const auto& img = result.value();
    {
        std::ofstream f(output, std::ios::binary | std::ios::trunc);
        if (!f)
            return fail(err, ErrorCode::IoError, "cannot create output file: " + output);
        f.write(reinterpret_cast<const char*>(img.bytes.data()),
                static_cast<std::streamsize>(img.bytes.size()));
        if (!f)
            return fail(err, ErrorCode::IoError, "cannot write output file: " + output);
    }
    const double total_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    if (as_json) {
        json j;
        j["schema_version"] = 1;
        j["model"] = model;
        j["mode"] = sub;
        j["output"] = output;
        j["width"] = img.width;
        j["height"] = img.height;
        j["steps"] = img.steps;
        j["preset"] = img.preset;
        j["bytes"] = img.bytes.size();
        j["backend"] = lm.backend;
        j["device"] = lm.device;
        j["load_ms"] = lm.load_ms;
        j["generation_ms"] = img.generation_ms;
        j["total_ms"] = total_ms;
        out << j.dump(2) << "\n";
    } else {
        out << "wrote " << output << " (" << img.width << "x" << img.height << ", "
            << static_cast<long>(img.generation_ms) << " ms)\n";
    }
    (void)engine.unload();
    return 0;
}

#endif  // HCS_HAVE_DIFFUSION

}  // namespace hcs::cli
