#include "hcs/cli/cli.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <ostream>
#include <sstream>

#include "benchmark_cmd.hpp"
#include "cache_cmd.hpp"
#include "chat_cmd.hpp"
#include "hcs/fs_util.hpp"
#include "hcs/model_registry.hpp"
#include "hcs/path_guard.hpp"
#include "hcs/result.hpp"
#include "hcs/version.hpp"
#include "image_cmd.hpp"
#include "train_cmd.hpp"
#include "vision_cmd.hpp"

#ifdef HCS_HAVE_API
#include <iostream>

#include "hcs/api/runtime.hpp"
#endif
#ifdef HCS_HAVE_MCP
#include "hcs/mcp/mcp_server.hpp"
#endif

#ifdef HCS_HAVE_HARDWARE
// Contract for the hardware module (src/hardware): writes the doctor report to `out`
// (JSON when `json`), returns an hcs exit code.
#include "hcs/hardware/doctor.hpp"
#endif

namespace hcs::cli {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr const char* kPlanned[] = {"tune"};

const char* kRootHelp =
    "HCSInfer - local-first AI runtime (text, vision, image generation)\n"
    "\n"
    "Usage: HCSInfer [--state-dir <dir>] <command> [options]\n"
    "\n"
    "Commands:\n"
    "  doctor [--json]               Hardware/runtime diagnostics (unknown values are reported as "
    "'unknown')\n"
    "  models list [--json]          List registered models\n"
    "  models scan <dir> [--json]    Read-only scan of a model folder (GGUF/safetensors headers validated)\n"
    "  models inspect <id>           Show a model manifest as JSON\n"
    "  models load|unload            Not in the one-shot CLI (a model lives in a running process): use\n"
    "                                `serve` (POST /hcs/v1/models/<id>/load), MCP, or --model on "
    "chat/vision/image\n"
    "  chat --model <path-or-id> [--prompt ...] [--backend cpu|vulkan] [--json]\n"
    "                                Text generation via llama.cpp (see `chat --help`)\n"
    "  image generate|edit ...       Image generation/edit via stable-diffusion.cpp (see `image --help`)\n"
    "  serve [--host ...]            Local HTTP API (OpenAI-compatible + /hcs/v1)\n"
    "  mcp stdio                     MCP server over stdio\n"
    "  vision --model <path-or-id> --image <file> --prompt ... [--mmproj <file>] [--json]\n"
    "                                Image analysis via llama.cpp libmtmd (see `vision --help`)\n"
    "  benchmark --suite quick|full --model <path-or-id> [--export file]\n"
    "                                Real text-model benchmark, JSON per AGENTS.md 21.1 (see `benchmark "
    "--help`)\n"
    "  train <subcommand>            Dataset validation, eval gate, adapter keep/rollback (see `train "
    "--help`)\n"
    "  cache stats|clear [--json]    Count / delete HCS-owned state-directory caches; `clear` needs --yes\n"
    "                                (see `cache --help`; model files are never touched)\n"
    "  tune                          Planned - not implemented yet (exit code 10)\n"
    "\n"
    "Global options:\n"
    "  --state-dir <dir>   State directory (default: $HCS_STATE_DIR, %LOCALAPPDATA%\\HCSInfer or "
    "~/.hcsinfer)\n"
    "  --version           Print version\n"
    "  --help, -h          Show help (works at every level)\n"
    "\n"
    "Exit codes: 0 ok, 2 invalid argument, 3 model error, 4 backend error, 5 out of memory,\n"
    "6 cancelled, 7 not found, 8 I/O error, 9 security violation, 10 unsupported,\n"
    "11 invalid state, 12 already exists, 13 busy, 14 timeout, 70 internal error.\n";

const char* kDoctorHelp =
    "Usage: HCSInfer doctor [--json] [--no-vulkan] [--storage-root <path>] [--read-file <file>]\n"
    "                       [--read-max-mib <n>] [--read-buffer-kib <n>] [--read-repeats <1-5>]\n"
    "\n"
    "Prints a read-only diagnostics report (CPU, RAM, OS, GPU/driver, Vulkan, storage).\n"
    "Never changes system settings. Undetectable values are reported as 'unknown'.\n"
    "  --json               Machine-readable report (schema_version 1)\n"
    "  --no-vulkan          CPU-only mode: vulkan-1.dll is not loaded at all\n"
    "  --storage-root <p>   Report volume / filesystem / bus type of an existing path\n"
    "  --read-file <f>      Read-only sequential read benchmark of an existing file (bounded buffer)\n"
    "  --read-max-mib <n>   Bytes read per sample, default 256 MiB\n"
    "  --read-buffer-kib <n> Read buffer, 64..65536 KiB, default 4096\n"
    "  --read-repeats <n>   Samples, 1..5, default 1 (cold/warm state stays 'unknown')\n";

const char* kModelsHelp =
    "Usage: HCSInfer models <subcommand>\n"
    "\n"
    "  list [--json]          List registered models\n"
    "  scan <dir> [--json]    Scan <dir> (read-only). <dir> becomes the approved root for this scan.\n"
    "  inspect <id>           Print the manifest of one model as JSON\n"
    "  load <id>, unload      Not in the one-shot CLI (exit code 10): pass --model to chat/vision/image, or "
    "load\n"
    "                         in a running `serve` / MCP process (POST /hcs/v1/models/<id>/load)\n";

const char* kModelsListHelp = "Usage: HCSInfer models list [--json]\n";
const char* kModelsScanHelp =
    "Usage: HCSInfer models scan <dir> [--json]\n"
    "\n"
    "Recursively (depth <= 8) finds *.gguf / *.safetensors, validates headers (extensions are not\n"
    "trusted), registers them in the state directory. Nothing is written to <dir>, hashed or loaded.\n";
const char* kModelsInspectHelp = "Usage: HCSInfer models inspect <id>\n";

bool has_flag(std::vector<std::string>& v, const std::string& flag) {
    auto it = std::find(v.begin(), v.end(), flag);
    if (it == v.end())
        return false;
    v.erase(it);
    return true;
}

bool wants_help(const std::vector<std::string>& v) {
    return std::any_of(v.begin(), v.end(), [](const std::string& a) { return a == "--help" || a == "-h"; });
}

int fail(std::ostream& err, const Error& e) {
    err << "error: " << e.message << " [" << to_string(e.code) << "]\n";
    return e.exit_code();
}
int fail(std::ostream& err, ErrorCode c, const std::string& msg) {
    return fail(err, Error(c, msg));
}

// Removes "<flag> <value>" from v. Returns false when the flag is present without a value.
bool take_option(std::vector<std::string>& v, const std::string& flag, std::string& value, bool& present) {
    present = false;
    auto it = std::find(v.begin(), v.end(), flag);
    if (it == v.end())
        return true;
    if (it + 1 == v.end())
        return false;
    value = *(it + 1);
    present = true;
    v.erase(it, it + 2);
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

Result<void> load_registry(ModelRegistry& reg, const fs::path& file) {
    std::error_code ec;
    if (!fs::exists(file, ec))
        return {};
    auto text = read_file_limited(file, 64u << 20);
    if (!text)
        return text.error();
    return reg.load_json(text.value());
}

int cmd_doctor(std::vector<std::string> args, std::ostream& out, std::ostream& err) {
    if (wants_help(args)) {
        out << kDoctorHelp;
        return 0;
    }
    const bool as_json = has_flag(args, "--json");
#ifdef HCS_HAVE_HARDWARE
    hcs::hardware::DoctorOptions opt;
    opt.json = as_json;
    opt.no_vulkan = has_flag(args, "--no-vulkan");
    for (const char* name :
         {"--storage-root", "--read-file", "--read-max-mib", "--read-buffer-kib", "--read-repeats"}) {
        std::string value;
        bool present = false;
        if (!take_option(args, name, value, present))
            return fail(err, ErrorCode::InvalidArgument, std::string(name) + " needs a value");
        if (!present)
            continue;
        const std::string n = name;
        if (n == "--storage-root") {
            opt.storage_root = value;
            continue;
        }
        if (n == "--read-file") {
            opt.read_file = value;
            continue;
        }
        if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos || value.size() > 9)
            return fail(err, ErrorCode::InvalidArgument, n + " needs a positive integer");
        const std::uint64_t num = std::stoull(value);
        if (n == "--read-max-mib")
            opt.read_max_bytes = num << 20;
        else if (n == "--read-buffer-kib")
            opt.read_buffer_bytes = static_cast<std::uint32_t>(std::min<std::uint64_t>(num, 1u << 20) << 10);
        else
            opt.read_repeats = static_cast<std::uint32_t>(num);
    }
    if (!args.empty())
        return fail(err, ErrorCode::InvalidArgument, "unexpected argument: " + args.front());
    return hcs::hardware::run_doctor(opt, out, err);
#else
    if (!args.empty())
        return fail(err, ErrorCode::InvalidArgument, "unexpected argument: " + args.front());
#ifdef _WIN32
    const char* os = "windows";
#else
    const char* os = "non-windows";
#endif
    if (as_json) {
        json j;
        j["schema_version"] = 1;
        j["hcsinfer_version"] = HCS_VERSION_STRING;
        j["hardware_module"] = "absent";
        j["os"] = os;
        for (const char* k :
             {"os_build", "cpu", "cpu_features", "gpu", "gpu_vendor_id", "gpu_device_id", "driver_version",
              "vulkan_api_version", "ram_bytes", "uma_reservation", "model_vault_drive"}) {
            j[k] = "unknown";
        }
        out << j.dump(2) << "\n";
    } else {
        out << "HCSInfer doctor (hardware module not built; values are unknown, not guessed)\n"
            << "  version:        " << HCS_VERSION_STRING << "\n"
            << "  os:             " << os << "\n"
            << "  cpu:            unknown\n"
            << "  gpu:            unknown\n"
            << "  vulkan:         unknown\n"
            << "  ram:            unknown\n"
            << "  model vault:    unknown\n";
    }
    return 0;
#endif
}

int cmd_models(std::vector<std::string> args, const std::string& state_override, std::ostream& out,
               std::ostream& err) {
    if (args.empty()) {
        err << kModelsHelp;
        return exit_code(ErrorCode::InvalidArgument);
    }
    const std::string sub = args.front();
    args.erase(args.begin());
    if (sub == "--help" || sub == "-h") {
        out << kModelsHelp;
        return 0;
    }

    const fs::path registry_file = state_dir(state_override) / "registry.json";

    if (sub == "list") {
        if (wants_help(args)) {
            out << kModelsListHelp;
            return 0;
        }
        const bool as_json = has_flag(args, "--json");
        if (!args.empty())
            return fail(err, ErrorCode::InvalidArgument, "unexpected argument: " + args.front());
        ModelRegistry reg;
        if (auto r = load_registry(reg, registry_file); !r)
            return fail(err, r.error());
        if (as_json) {
            out << reg.to_json();
            return 0;
        }
        const auto models = reg.list();
        if (models.empty()) {
            out << "No models registered. Use: HCSInfer models scan <dir>\n";
            return 0;
        }
        out << std::left << std::setw(32) << "ID" << std::setw(13) << "FORMAT" << std::setw(9) << "KIND"
            << std::setw(14) << "SIZE (MiB)" << "PATH\n";
        for (const auto& m : models) {
            out << std::left << std::setw(32) << m.id << std::setw(13) << to_string(m.format) << std::setw(9)
                << to_string(m.kind) << std::setw(14) << (m.size_bytes >> 20) << m.path << "\n";
        }
        return 0;
    }
    if (sub == "scan") {
        if (wants_help(args)) {
            out << kModelsScanHelp;
            return 0;
        }
        const bool as_json = has_flag(args, "--json");
        if (args.size() != 1)
            return fail(err, ErrorCode::InvalidArgument, "scan requires exactly one <dir>");
        PathGuard guard;
        if (auto r = guard.add_root(args[0]); !r)
            return fail(err, r.error());
        ModelRegistry reg;
        if (auto r = load_registry(reg, registry_file); !r)
            return fail(err, r.error());
        auto report = reg.scan(args[0], guard);
        if (!report)
            return fail(err, report.error());
        if (auto w = atomic_write_file(registry_file, reg.to_json()); !w)
            return fail(err, w.error());
        const ScanReport& rep = report.value();
        if (as_json) {
            json j;
            j["added"] = rep.added;
            j["updated"] = rep.updated;
            j["skipped"] = rep.skipped;
            j["messages"] = rep.messages;
            out << j.dump(2) << "\n";
        } else {
            out << "scan complete: " << rep.added << " added, " << rep.updated << " updated, " << rep.skipped
                << " skipped\n";
            for (const auto& m : rep.messages) out << "  " << m << "\n";
        }
        return 0;
    }
    if (sub == "inspect") {
        if (wants_help(args)) {
            out << kModelsInspectHelp;
            return 0;
        }
        if (args.size() != 1)
            return fail(err, ErrorCode::InvalidArgument, "inspect requires exactly one <id>");
        ModelRegistry reg;
        if (auto r = load_registry(reg, registry_file); !r)
            return fail(err, r.error());
        auto m = reg.get(args[0]);
        if (!m)
            return fail(err, m.error());
        // Reuse registry JSON for one model: extract it.
        json all = json::parse(reg.to_json());
        for (const auto& e : all["models"]) {
            if (e["id"] == args[0]) {
                out << e.dump(2) << "\n";
                return 0;
            }
        }
        return fail(err, ErrorCode::Internal, "registry inconsistency");
    }
    if (sub == "load" || sub == "unload") {
        if (wants_help(args)) {
            out << kModelsHelp;
            return 0;
        }
        return fail(
            err, ErrorCode::Unsupported,
            "models " + sub +
                " is not available in the one-shot CLI: pass --model to chat/vision/image, or use `serve` "
                "(POST /hcs/v1/models/<id>/load) or MCP");
    }
    return fail(err, ErrorCode::InvalidArgument, "unknown models subcommand: " + sub);
}

}  // namespace

std::string root_help() {
    return kRootHelp;
}

int run(const std::vector<std::string>& raw_args, std::ostream& out, std::ostream& err) {
    std::vector<std::string> args;
    std::string state_override;
    for (std::size_t i = 0; i < raw_args.size(); ++i) {
        if (raw_args[i] == "--state-dir") {
            if (i + 1 >= raw_args.size())
                return fail(err, ErrorCode::InvalidArgument, "--state-dir needs a value");
            state_override = raw_args[++i];
        } else {
            args.push_back(raw_args[i]);
        }
    }
    if (args.empty()) {
        err << kRootHelp;
        return exit_code(ErrorCode::InvalidArgument);
    }
    const std::string cmd = args.front();
    if (cmd == "--help" || cmd == "-h" || cmd == "help") {
        out << kRootHelp;
        return 0;
    }
    if (cmd == "--version") {
        out << "HCSInfer " << HCS_VERSION_STRING << "\n";
        return 0;
    }
    args.erase(args.begin());

    if (cmd == "doctor")
        return cmd_doctor(std::move(args), out, err);
    if (cmd == "chat")
        return run_chat(std::move(args), state_override, out, err);
    if (cmd == "benchmark")
        return run_benchmark(std::move(args), state_override, out, err);
    if (cmd == "cache")
        return run_cache(std::move(args), state_dir(state_override), out, err);
    if (cmd == "vision")
        return run_vision(std::move(args), state_override, out, err);
    if (cmd == "image")
        return run_image(std::move(args), state_override, out, err);
    if (cmd == "models")
        return cmd_models(std::move(args), state_override, out, err);
    if (cmd == "train") {
#ifdef HCS_HAVE_TRAIN
        return run_train(std::move(args), state_dir(state_override), out, err);
#else
        return fail(err, ErrorCode::Unsupported, "'train' is not available in this build");
#endif
    }
#ifdef HCS_HAVE_API
    if (cmd == "serve")
        return hcs::api::run_serve(args, state_dir(state_override), out, err);
#endif
#ifdef HCS_HAVE_MCP
    if (cmd == "mcp") {
        if (args.empty() || args[0] == "--help" || args[0] == "-h") {
            out << "Usage: HCSInfer mcp stdio [options]\n\nRun `HCSInfer mcp stdio --help` for details.\n";
            return args.empty() ? exit_code(ErrorCode::InvalidArgument) : 0;
        }
        if (args[0] != "stdio")
            return fail(err, ErrorCode::InvalidArgument,
                        "unknown mcp transport: " + args[0] + " (only 'stdio' is implemented)");
        args.erase(args.begin());
        return hcs::mcp::run_stdio(args, state_dir(state_override), std::cin, out, err);
    }
#endif
    if (std::find(std::begin(kPlanned), std::end(kPlanned), cmd) != std::end(kPlanned)) {
        if (wants_help(args)) {
            out << "Usage: HCSInfer " << cmd
                << " ...\n\nPlanned: this command is not implemented yet (exit code 10).\n";
            return 0;
        }
        return fail(err, ErrorCode::Unsupported, "'" + cmd + "' is planned and not implemented yet");
    }
    err << "error: unknown command '" << cmd << "'\n\n" << kRootHelp;
    return exit_code(ErrorCode::InvalidArgument);
}

}  // namespace hcs::cli
