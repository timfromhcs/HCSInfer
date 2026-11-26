// `HCSInfer merge` - streaming tensor-wise model merge (linear, SLERP, TIES, DARE, LoRA-into-base).
#include "merge_cmd.hpp"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <exception>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <ostream>
#include <sstream>

#include "hcs/merge.hpp"
#include "hcs/path_guard.hpp"
#include "hcs/result.hpp"

namespace hcs::cli {

namespace fs = std::filesystem;
using nlohmann::json;

const char* merge_help() {
    return "Usage: HCSInfer merge --method <m> --base <file> --model <file> [--model <file> ...] --out "
           "<file> "
           "[options]\n"
           "\n"
           "Tensor-wise, streaming merge of models with identical architecture (same tensor names and "
           "shapes).\n"
           "Inputs and output must share one container (safetensors or GGUF). Only float tensors "
           "(F32/F16/BF16)\n"
           "are merged; GGUF files containing quantized tensors (Q4_K, Q8_0, ...) are rejected: quantized\n"
           "formats are not merged in V1.1. Peak RAM is a few chunk buffers (plus the largest tensor for "
           "TIES).\n"
           "\n"
           "Methods:\n"
           "  linear   weighted average of the base and every --model (n+1 --weights, default equal)\n"
           "  slerp    spherical interpolation between the base and exactly one --model (--t 0..1)\n"
           "  ties     base + task models (--density, --lambda, one --weights per --model)\n"
           "  dare     base + task models with random drop + rescale (--drop-rate, --seed, --lambda)\n"
           "  lora     merge ONE LoRA file into the base (--weights <strength>, --alpha when the file has "
           "none)\n"
           "\n"
           "Options:\n"
           "  --weights a,b,...      Comma separated positive weights (see method)\n"
           "  --t <f>                SLERP position, default 0.5\n"
           "  --density <f>          TIES: fraction of each task vector kept, (0,1], default 0.5\n"
           "  --lambda <f>           TIES/DARE: scale of the merged task vector, default 1\n"
           "  --drop-rate <f>        DARE: drop probability, [0,1), default 0.5\n"
           "  --seed <n>             DARE: deterministic mask seed, default 0\n"
           "  --alpha <f>            LoRA: alpha when the file carries none (0 = rank, scale 1)\n"
           "  --max-ram-mib <n>      Cap for the largest transient buffer, default 2048\n"
           "  --force                Overwrite an existing output\n"
           "  --no-manifest          Do not write <out>.merge.json (method, params, SHA-256 of the output)\n"
           "  --json                 Machine-readable report on stdout\n"
           "\n"
           "Safety: every path you pass becomes an approved location for this one run (like `models scan`); "
           "the\n"
           "output must NOT be on drive C: and never replaces an input. Nothing is written when validation "
           "fails;\n"
           "a failed or cancelled merge leaves no output file. Ctrl-C cancels.\n";
}

namespace {

std::atomic<bool> g_interrupted{false};
CancellationSource* g_cancel = nullptr;
extern "C" void on_merge_sigint(int) {
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

bool parse_double(const std::string& s, double& out) {
    try {
        std::size_t pos = 0;
        out = std::stod(s, &pos);
        return pos == s.size();
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace

int run_merge_cmd(std::vector<std::string> args, std::ostream& out, std::ostream& err) {
    if (std::any_of(args.begin(), args.end(),
                    [](const std::string& a) { return a == "--help" || a == "-h"; })) {
        out << merge_help();
        return 0;
    }
    if (args.empty()) {
        err << merge_help();
        return exit_code(ErrorCode::InvalidArgument);
    }
    bool bad = false;
    std::string method_s, base, output, weights_s, v_t, v_density, v_lambda, v_drop, v_seed, v_alpha, v_ram;
    std::vector<std::string> models;
    take_value(args, "--method", method_s, bad);
    take_value(args, "--base", base, bad);
    take_value(args, "--out", output, bad);
    take_value(args, "--weights", weights_s, bad);
    take_value(args, "--t", v_t, bad);
    take_value(args, "--density", v_density, bad);
    take_value(args, "--lambda", v_lambda, bad);
    take_value(args, "--drop-rate", v_drop, bad);
    take_value(args, "--seed", v_seed, bad);
    take_value(args, "--alpha", v_alpha, bad);
    take_value(args, "--max-ram-mib", v_ram, bad);
    for (std::string m; take_value(args, "--model", m, bad);) models.push_back(m);
    const bool force = take_flag(args, "--force");
    const bool no_manifest = take_flag(args, "--no-manifest");
    const bool as_json = take_flag(args, "--json");
    if (bad)
        return fail(err, ErrorCode::InvalidArgument, "an option is missing its value");
    if (!args.empty())
        return fail(err, ErrorCode::InvalidArgument, "unexpected argument: " + args.front());
    if (method_s.empty() || base.empty() || output.empty())
        return fail(err, ErrorCode::InvalidArgument, "--method, --base and --out are required");

    auto method = merge::parse_method(method_s);
    if (!method)
        return fail(err, method.error());

    merge::Request req;
    req.base = base;
    for (const auto& m : models) req.models.emplace_back(m);
    req.out = output;
    req.overwrite = force;
    req.write_manifest = !no_manifest;
    req.params.method = method.value();
    {
        std::istringstream ws(weights_s);
        for (std::string tok; std::getline(ws, tok, ',');) {
            double v = 0;
            if (!parse_double(tok, v))
                return fail(err, ErrorCode::InvalidArgument, "--weights must be comma separated numbers");
            req.params.weights.push_back(v);
        }
    }
    auto num = [&](const std::string& s, const char* name, double& dst) -> bool {
        if (s.empty())
            return true;
        if (!parse_double(s, dst)) {
            err << "error: " << name << " needs a number [invalid_argument]\n";
            return false;
        }
        return true;
    };
    double seed_d = 0.0, ram_mib = 2048.0;
    if (!num(v_t, "--t", req.params.t) || !num(v_density, "--density", req.params.density) ||
        !num(v_lambda, "--lambda", req.params.lambda) || !num(v_drop, "--drop-rate", req.params.drop_rate) ||
        !num(v_alpha, "--alpha", req.params.lora_alpha) || !num(v_seed, "--seed", seed_d) ||
        !num(v_ram, "--max-ram-mib", ram_mib))
        return exit_code(ErrorCode::InvalidArgument);
    if (seed_d < 0 || seed_d > 1.8e19)
        return fail(err, ErrorCode::InvalidArgument, "--seed must be a non-negative integer");
    req.params.seed = v_seed.empty() ? 0 : std::stoull(v_seed);
    if (!(ram_mib >= 16.0 && ram_mib <= 1048576.0))
        return fail(err, ErrorCode::InvalidArgument, "--max-ram-mib must be 16..1048576");
    req.max_ram_bytes = static_cast<std::uint64_t>(ram_mib) << 20;

    // The caller typed these paths: their folders are the approved roots of THIS run (same model as
    // `models scan <dir>`). Drive C: for the output and "output is an input" are still refused by run_merge.
    PathGuard guard;
    auto add_dir = [&](const fs::path& file) -> Result<void> {
        fs::path dir = file.parent_path();
        if (dir.empty())
            dir = fs::current_path();
        std::error_code ec;
        if (!fs::is_directory(dir, ec))
            return Error(ErrorCode::NotFound, "directory does not exist: " + dir.string());
        return guard.add_root(dir);
    };
    if (auto r = add_dir(req.base); !r)
        return fail(err, r.error());
    for (const auto& m : req.models)
        if (auto r = add_dir(m); !r)
            return fail(err, r.error());
    if (auto r = add_dir(req.out); !r)
        return fail(err, r.error());

    std::signal(SIGINT, on_merge_sigint);
    CancellationSource source;
    g_cancel = &source;
    req.token = source.token();
    req.progress = [&](double f, const std::string& name) {
        if (as_json)
            return;
        err << "\r[hcsinfer] merge " << static_cast<int>(f * 100.0) << "% " << name.substr(0, 48)
            << "          " << std::flush;
    };
    auto rep = merge::run_merge(req, guard);
    g_cancel = nullptr;
    if (!as_json)
        err << "\n";
    if (!rep) {
        if (rep.error().code == ErrorCode::Cancelled || g_interrupted.load()) {
            err << "cancelled\n";
            return exit_code(ErrorCode::Cancelled);
        }
        return fail(err, rep.error());
    }
    const auto& r = rep.value();
    if (as_json) {
        json j;
        j["schema_version"] = 1;
        j["format"] = r.format;
        j["method"] = r.method;
        j["output"] = r.output.string();
        j["manifest"] = r.manifest.string();
        j["tensors_total"] = r.tensors_total;
        j["tensors_merged"] = r.tensors_merged;
        j["tensors_copied"] = r.tensors_copied;
        j["bytes_written"] = r.bytes_written;
        j["peak_buffer_bytes"] = r.peak_buffer_bytes;
        j["seconds"] = r.seconds;
        j["output_sha256"] = r.output_sha256;
        out << j.dump(2) << "\n";
    } else {
        out << "merged " << r.tensors_merged << " tensor(s), copied " << r.tensors_copied << ", wrote "
            << r.output.string() << " (" << r.bytes_written << " bytes, sha256 " << r.output_sha256 << ")\n";
    }
    return 0;
}

}  // namespace hcs::cli
