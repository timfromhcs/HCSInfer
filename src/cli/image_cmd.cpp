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
#include "hcs/image_profiles.hpp"
#include "hcs/lora.hpp"
#include "hcs/model_registry.hpp"
#include "hcs/path_guard.hpp"
#include "hcs/result.hpp"
#include "hcs/state_dir.hpp"
#include "hcs/upscale_ort.hpp"

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
           "  --params-backend <b>   cpu (default) | disk: where module weights live between uses; disk is "
           "a\n"
           "                         capacity fallback and can be slower\n"
           "  --cfg <f>              Text CFG scale; 0 = engine/model default\n"
           "  --allow-low-quality    Render sizes/steps below the model family's quality envelope (SD1.5: "
           "short\n"
           "                         side < 384 px or < 8 steps is rejected otherwise; output is a smoke "
           "test)\n"
           "  --lora <id[:weight]>   Apply a LoRA from the library (repeatable, max 4). Weight 0<|w|<=2, "
           "default\n"
           "                         = the manifest's recommended weight. A LoRA for another base family\n"
           "                         (e.g. SD1.5 on FLUX) is rejected with a structured error.\n"
           "  --lora-dir <dir>       LoRA library folder with manifest.json (default: <model "
           "root>/lora/image,\n"
           "                         searched upwards from the model file; see docs/lora.md)\n"
           "  --upscale <off|4k>     4k: tiled 4x ONNX super-resolution + Lanczos to a 3840 px long edge\n"
           "                         (default off). Needs onnxruntime.dll + a .onnx model, otherwise exit "
           "10.\n"
           "  --upscale-dir <dir>    Folder with the .onnx model and onnxruntime.dll (default: <model "
           "root>/upscale)\n"
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
    return hcs::resolve_state_dir(override_dir);
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

// <ancestor of the model>/<leaf> (e.g. D:\HCSModels\lora\image), searched upwards at most 5 levels.
fs::path find_library_dir(const fs::path& model_file, const char* leaf1, const char* leaf2) {
    std::error_code ec;
    fs::path dir = model_file.parent_path();
    for (int i = 0; i < 5 && !dir.empty(); ++i, dir = dir.parent_path()) {
        const fs::path cand = leaf2 ? dir / leaf1 / leaf2 : dir / leaf1;
        if (fs::is_directory(cand, ec))
            return cand;
        if (dir == dir.parent_path())
            break;
    }
    return {};
}

// "id" or "id:weight". Weight bounds are validated by the library (finite, non-zero, |w| <= 2).
Result<LoraLibrary::Ref> parse_lora_arg(const std::string& arg) {
    LoraLibrary::Ref ref;
    const auto colon = arg.rfind(':');
    if (colon == std::string::npos) {
        ref.id = arg;
    } else {
        ref.id = arg.substr(0, colon);
        try {
            std::size_t pos = 0;
            const std::string w = arg.substr(colon + 1);
            ref.weight = std::stof(w, &pos);
            if (pos != w.size())
                throw std::invalid_argument("trailing");
            ref.weight_given = true;
        } catch (const std::exception&) {
            return Error(ErrorCode::InvalidArgument, "--lora weight in '" + arg + "' is not a number");
        }
    }
    if (ref.id.empty())
        return Error(ErrorCode::InvalidArgument, "--lora needs an id (id[:weight])");
    return ref;
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
    std::string lora_dir, upscale_mode, upscale_dir;
    std::vector<std::string> lora_args;
    take_value(args, "--lora-dir", lora_dir, bad);
    take_value(args, "--upscale", upscale_mode, bad);
    take_value(args, "--upscale-dir", upscale_dir, bad);
    for (std::string v; take_value(args, "--lora", v, bad);) lora_args.push_back(v);
    const bool as_json = take_flag(args, "--json");
    const bool force = take_flag(args, "--force");
    const bool allow_low_quality = take_flag(args, "--allow-low-quality");
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
    req.allow_low_quality = allow_low_quality;
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

    if (upscale_mode != "" && upscale_mode != "off" && upscale_mode != "4k")
        return fail(err, ErrorCode::InvalidArgument, "--upscale must be off or 4k");
    const bool want_upscale = upscale_mode == "4k";

    // LoRAs: ids only, resolved through the library (paths inside its approved root, family and weight
    // checked BEFORE the model is loaded). The engine repeats the family check against the loaded weights.
    if (!lora_args.empty()) {
        if (lora_args.size() > kMaxLorasPerRequest)
            return fail(err, ErrorCode::InvalidArgument,
                        "at most " + std::to_string(kMaxLorasPerRequest) + " --lora options");
        std::vector<LoraLibrary::Ref> refs;
        for (const auto& a : lora_args) {
            auto ref = parse_lora_arg(a);
            if (!ref)
                return fail(err, ref.error());
            refs.push_back(ref.value());
        }
        fs::path dir =
            lora_dir.empty() ? find_library_dir(lr.model_path, "lora", "image") : fs::path(lora_dir);
        if (dir.empty())
            return fail(err, ErrorCode::NotFound,
                        "no LoRA library: pass --lora-dir or create <model root>/lora/image/manifest.json");
        if (dir.string().find("..") != std::string::npos)
            return fail(err, ErrorCode::SecurityViolation, "--lora-dir must not contain '..'");
        PathGuard guard;
        if (auto r = guard.add_root(dir); !r)
            return fail(err, r.error());
        LoraLibrary lib;
        if (auto r = lib.scan(guard, dir); !r)
            return fail(err, r.error());
        auto specs =
            lib.resolve(refs, guess_image_family(model + " " + fs::path(lr.model_path).filename().string()),
                        /*allow_unknown_family=*/true);
        if (!specs)
            return fail(err, specs.error());
        req.loras = std::move(specs.value());
    }

    // Upscaler: constructed now (cheap), checked before the render so a missing runtime/model never costs a
    // long generation.
    std::unique_ptr<upscale::OrtUpscaleEngine> upscaler;
    if (want_upscale) {
        upscale::OrtConfig oc;
        fs::path dir =
            upscale_dir.empty() ? find_library_dir(lr.model_path, "upscale", nullptr) : fs::path(upscale_dir);
        std::error_code uec;
        if (!dir.empty() && fs::is_directory(dir, uec)) {
            oc.runtime_dirs.push_back(dir);
            oc.model_path = upscale::find_upscale_model(dir);
        }
        if (const auto exe = upscale::executable_dir(); !exe.empty())
            oc.runtime_dirs.push_back(exe);
        upscaler = std::make_unique<upscale::OrtUpscaleEngine>(std::move(oc));
        if (auto r = upscaler->ready(); !r)
            return fail(err, Error(r.error().code, "--upscale 4k is not available: " + r.error().message));
    }

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

    auto img = std::move(result.value());
    json upscale_json;
    if (upscaler) {
        upscale::Stats st;
        g_cancel = &source;  // Ctrl-C also cancels the upscale
        auto up = upscale::upscale_png(
            *upscaler, img.bytes, upscale::Options{}, source.token(),
            [&](double f) {
                if (!as_json)
                    err << "\r[hcsinfer] upscale " << static_cast<int>(f * 100.0) << "%          "
                        << std::flush;
            },
            &st);
        g_cancel = nullptr;
        if (!as_json)
            err << "\n";
        if (!up) {
            if (up.error().code == ErrorCode::Cancelled || g_interrupted.load()) {
                err << "cancelled\n";
                return exit_code(ErrorCode::Cancelled);
            }
            // The render is kept; the failure is reported, never hidden.
            err << "warning: upscale failed, writing the un-upscaled image: " << up.error().message << " ["
                << to_string(up.error().code) << "]\n";
            upscale_json = {{"status", "failed"}, {"error", up.error().message}};
        } else if (st.skipped) {
            upscale_json = {{"status", "skipped"}, {"note", st.skip_reason}};
        } else {
            img.bytes = std::move(up.value());
            img.width = st.out_w;
            img.height = st.out_h;
            upscale_json = {{"status", "done"},        {"from_width", st.src_w},
                            {"from_height", st.src_h}, {"ms", st.model_ms + st.resample_ms},
                            {"provider", st.provider}, {"tiles", st.tiles}};
        }
    }
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
        if (!req.loras.empty()) {
            j["loras"] = json::array();
            for (const auto& l : req.loras) j["loras"].push_back({{"id", l.id}, {"weight", l.weight}});
        }
        if (!upscale_json.is_null())
            j["upscale"] = upscale_json;
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
