// `HCSInfer chat` - routes through the engine adapter (never raw llama_* calls).
#include "chat_cmd.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
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

const char* chat_help() {
    return "Usage: HCSInfer chat --model <path-or-id> [--prompt \"...\"] [options]\n"
           "\n"
           "Runs a GGUF text model through the llama.cpp engine adapter. Without --prompt, reads one prompt "
           "per\n"
           "line from stdin (each line is an independent single-turn request; empty line or /exit ends).\n"
           "\n"
           "Options:\n"
           "  --model <path-or-id>   GGUF file path, or a model id from `models scan` (required)\n"
           "  --prompt <text>        Prompt (user message)\n"
           "  --system <text>        System prompt\n"
           "  --backend <b>          auto (default) | cpu | vulkan. vulkan fails (exit 4/10) if "
           "unavailable;\n"
           "                         there is no silent fallback when cpu/vulkan is requested explicitly\n"
           "  --device <name>        GPU device name/description substring (e.g. Vulkan0)\n"
           "  --gpu-layers <n>       Layers to offload (default: all when using a GPU)\n"
           "  --ctx <n>              Context tokens (default 4096)\n"
           "  --max-tokens <n>       Max generated tokens (default 256)\n"
           "  --temp <t>             Temperature; 0 = greedy (default 0.8)\n"
           "  --seed <n>             Sampling seed\n"
           "  --threads <n>          CPU threads (default: physical cores)\n"
           "  --load-mode <m>        auto|none|mmap|mlock|mmap+mlock|direct_io\n"
           "  --raw                  Do not apply the model's chat template\n"
           "  --json                 Machine-readable result (text + metrics) on stdout\n"
           "\n"
           "Ctrl-C cancels the running generation (exit code 6).\n";
}

#ifndef HCS_HAVE_LLAMA

int run_chat(std::vector<std::string> args, const std::string&, std::ostream& out, std::ostream& err) {
    for (const auto& a : args) {
        if (a == "--help" || a == "-h") {
            out << chat_help();
            return 0;
        }
    }
    err << "error: this build has no llama engine (external/llama.cpp missing) [unsupported]\n";
    return exit_code(ErrorCode::Unsupported);
}

#else

namespace {

std::atomic<bool> g_interrupted{false};
extern "C" void on_sigint(int) {
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

}  // namespace

int run_chat(std::vector<std::string> args, const std::string& state_override, std::ostream& out,
             std::ostream& err) {
    if (std::any_of(args.begin(), args.end(),
                    [](const std::string& a) { return a == "--help" || a == "-h"; })) {
        out << chat_help();
        return 0;
    }
    std::string model, prompt, system, backend = "auto", device, load_mode, v_gpu, v_ctx, v_max, v_temp,
                                       v_seed, v_thr;
    bool bad = false;
    const bool has_prompt = take_value(args, "--prompt", prompt, bad);
    take_value(args, "--model", model, bad);
    take_value(args, "--system", system, bad);
    take_value(args, "--backend", backend, bad);
    take_value(args, "--device", device, bad);
    take_value(args, "--load-mode", load_mode, bad);
    take_value(args, "--gpu-layers", v_gpu, bad);
    take_value(args, "--ctx", v_ctx, bad);
    take_value(args, "--max-tokens", v_max, bad);
    take_value(args, "--temp", v_temp, bad);
    take_value(args, "--seed", v_seed, bad);
    take_value(args, "--threads", v_thr, bad);
    const bool raw = take_flag(args, "--raw");
    const bool as_json = take_flag(args, "--json");
    if (bad)
        return fail(err, ErrorCode::InvalidArgument, "an option is missing its value");
    if (!args.empty())
        return fail(err, ErrorCode::InvalidArgument, "unexpected argument: " + args.front());
    if (model.empty())
        return fail(err, ErrorCode::InvalidArgument, "--model is required");

    LoadRequest lr;
    TextRequest tr;
    try {
        if (!v_gpu.empty())
            lr.gpu_layers = std::stoi(v_gpu);
        if (!v_ctx.empty())
            lr.context_tokens = static_cast<std::uint32_t>(std::stoul(v_ctx));
        if (!v_thr.empty())
            lr.threads = std::stoi(v_thr);
        if (!v_max.empty())
            tr.max_tokens = static_cast<std::uint32_t>(std::stoul(v_max));
        if (!v_temp.empty())
            tr.temperature = std::stof(v_temp);
        if (!v_seed.empty())
            tr.seed = std::stoull(v_seed);
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
    tr.system_prompt = system;
    tr.apply_chat_template = !raw;

    auto path = resolve_model(model, state_override);
    if (!path)
        return fail(err, path.error());
    lr.model_path = path.value();
    lr.model_id = model;

    JobQueue queue;
    engines::LlamaTextEngine engine(queue);
    if (auto r = engine.load(lr); !r)
        return fail(err, r.error());
    const EngineMetrics lm = engine.metrics();
    if (!as_json) {
        err << "[hcsinfer] backend=" << lm.backend << " device=" << lm.device
            << " load_ms=" << static_cast<long>(lm.load_ms) << " offloaded_layers=" << lm.offloaded_layers
            << "/" << lm.total_layers << "\n";
    }

    std::signal(SIGINT, on_sigint);
    json results = json::array();
    int exit_rc = 0;

    auto run_one = [&](const std::string& p) -> int {
        tr.prompt = p;
        std::mutex text_mu;
        std::string text;
        auto id = engine.generate(tr, [&](const JobEvent& ev) {
            if (ev.kind != JobEventKind::Output)
                return;
            std::lock_guard l(text_mu);
            text += ev.text;
            if (!as_json) {
                out << ev.text;
                out.flush();
            }
        });
        if (!id)
            return fail(err, id.error());
        bool cancelled_by_signal = false;
        while (!queue.wait(id.value(), std::chrono::milliseconds(50))) {
            if (g_interrupted.load() && !cancelled_by_signal) {
                cancelled_by_signal = true;
                (void)queue.cancel(id.value());
            }
        }
        auto st = queue.status(id.value());
        if (!st)
            return fail(err, st.error());
        const EngineMetrics m = engine.metrics();
        if (!as_json)
            out << "\n";
        json j;
        j["prompt_chars"] = p.size();  // prompt content is never echoed or logged
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
        results.push_back(j);
        if (!as_json) {
            err << "[hcsinfer] ttft_ms=" << static_cast<long>(m.ttft_ms)
                << " prompt_tps=" << m.prompt_tokens_per_second
                << " decode_tps=" << m.decode_tokens_per_second << "\n";
        }
        if (st.value().state == JobState::Failed)
            return fail(err, *st.value().error);
        if (st.value().state == JobState::Cancelled) {
            err << "cancelled\n";
            return exit_code(ErrorCode::Cancelled);
        }
        return 0;
    };

    if (has_prompt) {
        exit_rc = run_one(prompt);
    } else {
        std::string line;
        while (!g_interrupted.load() && std::getline(std::cin, line)) {
            if (line.empty() || line == "/exit")
                break;
            exit_rc = run_one(line);
            if (exit_rc != 0)
                break;
        }
    }

    if (as_json) {
        json j;
        j["schema_version"] = 1;
        j["model"] = model;
        j["backend"] = lm.backend;
        j["device"] = lm.device;
        j["offloaded_layers"] = lm.offloaded_layers;
        j["total_layers"] = lm.total_layers;
        j["results"] = results;
        out << j.dump(2) << "\n";
    }
    (void)engine.unload();
    return exit_rc;
}

#endif  // HCS_HAVE_LLAMA

}  // namespace hcs::cli
