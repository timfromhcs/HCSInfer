// `HCSInfer image upscale --input <png> --output <png>`: the 4K upscaler on an existing image. No diffusion
// engine is involved; only the ONNX Runtime backend (hcs/upscale_ort.hpp) and the tiling pipeline.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <ostream>
#include <string>

#include "hcs/fs_util.hpp"
#include "hcs/result.hpp"
#include "hcs/upscale_ort.hpp"
#include "image_cmd.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format off
#include <psapi.h>
// clang-format on
#endif

namespace hcs::cli {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::atomic<bool> g_up_interrupted{false};
CancellationSource* g_up_cancel = nullptr;

extern "C" void on_upscale_sigint(int) {
    g_up_interrupted.store(true);
    if (g_up_cancel)
        g_up_cancel->request_cancel();
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

bool parse_u32(const std::string& s, std::uint32_t& out) {
    if (s.empty() || s.size() > 9)
        return false;
    std::uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9')
            return false;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    out = static_cast<std::uint32_t>(v);
    return true;
}

std::uint64_t peak_working_set_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS c{};
    c.cb = sizeof(c);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &c, sizeof(c)))
        return static_cast<std::uint64_t>(c.PeakWorkingSetSize);
#endif
    return 0;
}

}  // namespace

const char* image_upscale_help() {
    return "Usage: HCSInfer image upscale --input <png> --output <png> [options]\n"
           "\n"
           "Upscales an existing PNG: a tiled 4x ONNX super-resolution pass (tile size chosen from the live\n"
           "memory budget, feather-blended overlaps) followed by a Lanczos-3 resample to exactly --target "
           "px\n"
           "on the long edge (default 3840). An image whose long edge is already >= the target is copied\n"
           "unchanged and reported as skipped. Experimental; needs onnxruntime.dll and a .onnx model in\n"
           "--upscale-dir (or next to HCSInfer.exe for the DLL), otherwise exit 10 [unsupported].\n"
           "\n"
           "Options:\n"
           "  --input <file>         Source PNG (8-bit; <= 32 MiB, <= 16 MP)\n"
           "  --output <file>        Output PNG (default: <input>-4k.png next to the input)\n"
           "  --upscale-dir <dir>    Folder with the .onnx model and onnxruntime.dll (default: searched "
           "upwards\n"
           "                         from the input for an `upscale` folder)\n"
           "  --target <px>          Long edge of the result, 64..16384 (default 3840)\n"
           "  --tile <px>            Force the tile size, 32..2048 (default: largest that fits the budget)\n"
           "  --overlap <px>         Tile overlap, default 16 (clamped to tile/4)\n"
           "  --pre-resize <px>      Lanczos the source to this long edge BEFORE the model pass (e.g. 960 so "
           "4x\n"
           "                         lands on 3840); more model time, default off\n"
           "  --budget-mib <n>      Memory budget for the planner (default 1024)\n"
           "  --threads <n>          ONNX Runtime intra-op threads (default: runtime default)\n"
           "  --provider <p>         auto (default; DirectML when usable, else CPU) | cpu | dml\n"
           "  --force                Overwrite an existing output file\n"
           "  --json                 Machine-readable result on stdout\n"
           "\n"
           "Ctrl-C cancels the running upscale (exit code 6).\n";
}

int run_image_upscale(std::vector<std::string> args, std::ostream& out, std::ostream& err) {
    if (std::any_of(args.begin(), args.end(),
                    [](const std::string& a) { return a == "--help" || a == "-h"; })) {
        out << image_upscale_help();
        return 0;
    }
    std::string v_pre;
    std::string input, output, dir, v_target, v_tile, v_overlap, v_budget, v_threads, provider = "auto";
    bool bad = false;
    take_value(args, "--input", input, bad);
    take_value(args, "--output", output, bad);
    take_value(args, "--upscale-dir", dir, bad);
    take_value(args, "--target", v_target, bad);
    take_value(args, "--tile", v_tile, bad);
    take_value(args, "--pre-resize", v_pre, bad);
    take_value(args, "--overlap", v_overlap, bad);
    take_value(args, "--budget-mib", v_budget, bad);
    take_value(args, "--threads", v_threads, bad);
    take_value(args, "--provider", provider, bad);
    const bool as_json = take_flag(args, "--json");
    const bool force = take_flag(args, "--force");
    if (bad)
        return fail(err, ErrorCode::InvalidArgument, "an option is missing its value");
    if (!args.empty())
        return fail(err, ErrorCode::InvalidArgument, "unexpected argument: " + args.front());
    if (input.empty())
        return fail(err, ErrorCode::InvalidArgument, "--input is required");
    if (provider != "auto" && provider != "cpu" && provider != "dml")
        return fail(err, ErrorCode::InvalidArgument, "--provider must be auto, cpu or dml");

    upscale::Options opt;
    upscale::OrtConfig oc;
    oc.provider = provider;
    std::uint32_t budget_mib = 1024;
    auto numeric = [&](const char* name, const std::string& text, std::uint32_t& dst) -> bool {
        if (text.empty())
            return true;
        if (parse_u32(text, dst))
            return true;
        fail(err, ErrorCode::InvalidArgument, std::string(name) + " needs a positive integer");
        return false;
    };
    if (!numeric("--target", v_target, opt.target_long_edge) || !numeric("--tile", v_tile, opt.tile) ||
        !numeric("--overlap", v_overlap, opt.overlap) ||
        !numeric("--pre-resize", v_pre, opt.pre_resize_long_edge) ||
        !numeric("--budget-mib", v_budget, budget_mib))
        return exit_code(ErrorCode::InvalidArgument);
    if (!v_threads.empty()) {
        std::uint32_t t = 0;
        if (!parse_u32(v_threads, t) || t == 0 || t > 256)
            return fail(err, ErrorCode::InvalidArgument, "--threads must be 1..256");
        oc.threads = static_cast<int>(t);
    }
    opt.memory_budget_bytes = static_cast<std::uint64_t>(budget_mib) << 20;

    if (input.find("..") != std::string::npos)
        return fail(err, ErrorCode::SecurityViolation, "--input must not contain '..'");
    std::error_code ec;
    if (!fs::is_regular_file(input, ec))
        return fail(err, ErrorCode::NotFound, "input image not found: " + input);
    const auto size = fs::file_size(input, ec);
    if (ec || size == 0)
        return fail(err, ErrorCode::InvalidArgument, "input image is empty or unreadable");
    if (size > (32u << 20))
        return fail(err, ErrorCode::SecurityViolation, "input image exceeds 32 MiB");
    const fs::path in_path = fs::weakly_canonical(input, ec);

    if (output.empty())
        output = (in_path.parent_path() / (in_path.stem().string() + "-4k.png")).string();
    if (output.find("..") != std::string::npos)
        return fail(err, ErrorCode::SecurityViolation, "--output must not contain '..'");
    if (!force && fs::exists(output, ec))
        return fail(err, ErrorCode::AlreadyExists, "output file exists (use --force): " + output);
    {
        const fs::path parent = fs::path(output).parent_path();
        if (!parent.empty() && !fs::is_directory(parent, ec))
            return fail(err, ErrorCode::NotFound, "output directory does not exist: " + parent.string());
    }

    // Runtime + model: --upscale-dir, else an `upscale` folder found upwards from the input; the DLL is also
    // looked up next to the executable. Nothing on PATH.
    fs::path udir = dir.empty() ? fs::path() : fs::path(dir);
    if (udir.empty()) {
        fs::path d = in_path.parent_path();
        for (int i = 0; i < 6 && !d.empty(); ++i, d = d.parent_path()) {
            if (fs::is_directory(d / "upscale", ec)) {
                udir = d / "upscale";
                break;
            }
            if (d == d.parent_path())
                break;
        }
    }
    if (!udir.empty() && fs::is_directory(udir, ec)) {
        oc.runtime_dirs.push_back(udir);
        oc.model_path = upscale::find_upscale_model(udir);
    }
    if (const auto exe = upscale::executable_dir(); !exe.empty())
        oc.runtime_dirs.push_back(exe);
    const int threads = oc.threads;  // 0 = ONNX Runtime default
    upscale::OrtUpscaleEngine engine(std::move(oc));
    if (auto r = engine.ready(); !r)
        return fail(err, Error(r.error().code, "image upscale is not available: " + r.error().message));

    auto bytes = read_file_limited(in_path, 32u << 20);
    if (!bytes)
        return fail(err, bytes.error());
    std::vector<std::uint8_t> png(bytes.value().begin(), bytes.value().end());

    std::signal(SIGINT, on_upscale_sigint);
    CancellationSource source;
    g_up_cancel = &source;
    upscale::Stats st;
    const auto t0 = std::chrono::steady_clock::now();
    auto up = upscale::upscale_png(
        engine, png, opt, source.token(),
        [&](double f) {
            if (!as_json)
                err << "\r[hcsinfer] upscale " << static_cast<int>(f * 100.0) << "%          " << std::flush;
        },
        &st);
    g_up_cancel = nullptr;
    if (!as_json)
        err << "\n";
    if (!up) {
        if (up.error().code == ErrorCode::Cancelled || g_up_interrupted.load()) {
            err << "cancelled\n";
            return exit_code(ErrorCode::Cancelled);
        }
        return fail(err, up.error());
    }
    const double total_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    {
        std::ofstream f(output, std::ios::binary | std::ios::trunc);
        if (!f)
            return fail(err, ErrorCode::IoError, "cannot create output file: " + output);
        f.write(reinterpret_cast<const char*>(up.value().data()),
                static_cast<std::streamsize>(up.value().size()));
        if (!f)
            return fail(err, ErrorCode::IoError, "cannot write output file: " + output);
    }
    if (as_json) {
        json j;
        j["schema_version"] = 1;
        j["input"] = input;
        j["output"] = output;
        j["status"] = st.skipped ? "skipped" : "done";
        if (st.skipped)
            j["note"] = st.skip_reason;
        j["from_width"] = st.src_w;
        j["from_height"] = st.src_h;
        j["width"] = st.out_w;
        j["height"] = st.out_h;
        j["provider"] = st.provider;
        j["model"] = st.model;
        j["runtime_version"] = engine.runtime_version();
        j["tiles"] = st.tiles;
        j["tile"] = st.tile;
        j["overlap"] = st.overlap;
        j["total_ms"] = total_ms;
        j["model_stage_ms"] = st.model_ms;
        j["tile_ms_median"] = st.tile_ms_median;
        j["tile_ms_max"] = st.tile_ms_max;
        j["blend_ms"] = st.blend_ms;
        j["pre_resize_ms"] = st.pre_resize_ms;
        j["pre_resize_long_edge"] = opt.pre_resize_long_edge;
        j["resample_ms"] = st.resample_ms;
        j["peak_bytes_estimate"] = st.peak_bytes_estimate;
        j["peak_working_set_bytes"] = peak_working_set_bytes();
        j["threads"] = threads;
        out << j.dump(2) << "\n";
    } else {
        out << (st.skipped ? "skipped (" + st.skip_reason + "): copied " : "wrote ") << output << " ("
            << st.out_w << "x" << st.out_h << ", " << static_cast<long>(total_ms) << " ms)\n";
    }
    return 0;
}

}  // namespace hcs::cli
