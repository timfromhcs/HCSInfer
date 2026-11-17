// Storage benchmark (AGENTS.md 11.5 / 21.1): raw vs LZ4 vs Zstandard chunk loading of one real model.
//
// Safety rules (AGENTS.md 0.2): the source file is only ever read. HCSMODEL containers are written
// ONLY into the explicitly given --scratch directory as NEW files whose names are generated here,
// and only those files are deleted afterwards (unless --keep).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "hcs/storage/model_reader.hpp"
#include "hcs/storage/writer.hpp"

#ifdef _WIN32
#include <intrin.h>
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#endif

using json = nlohmann::json;
using namespace hcs::storage;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

constexpr double kMiB = 1024.0 * 1024.0;

double secs(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

double percentile(std::vector<double> v, double p) {
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    const double idx = p * static_cast<double>(v.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(idx);
    const std::size_t hi = std::min(lo + 1, v.size() - 1);
    return v[lo] + (v[hi] - v[lo]) * (idx - static_cast<double>(lo));
}
double median(const std::vector<double>& v) {
    return percentile(v, 0.5);
}
double p90(const std::vector<double>& v) {
    return percentile(v, 0.9);
}

std::uint64_t peak_working_set() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return pmc.PeakWorkingSetSize;
#endif
    return 0;
}

std::string cpu_brand() {
#if defined(_WIN32) && (defined(_M_X64) || defined(_M_IX86))
    int r[4];
    char b[49] = {};
    __cpuid(r, 0x80000000);
    if (static_cast<unsigned>(r[0]) < 0x80000004u)
        return "unknown";
    for (int i = 0; i < 3; ++i) {
        __cpuid(r, 0x80000002 + i);
        std::memcpy(b + 16 * i, r, 16);
    }
    std::string s(b);
    const auto a = s.find_first_not_of(' ');
    return a == std::string::npos ? "unknown" : s.substr(a);
#else
    return "unknown";
#endif
}

std::string os_build() {
#ifdef _WIN32
    using Fn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll")) {
        if (auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(nt, "RtlGetVersion")))) {
            RTL_OSVERSIONINFOW vi{};
            vi.dwOSVersionInfoSize = sizeof(vi);
            if (fn(&vi) == 0)
                return "Windows " + std::to_string(vi.dwMajorVersion) + "." +
                       std::to_string(vi.dwMinorVersion) + "." + std::to_string(vi.dwBuildNumber);
        }
    }
#endif
    return "unknown";
}

std::uint64_t total_ram() {
#ifdef _WIN32
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms))
        return ms.ullTotalPhys;
#endif
    return 0;
}

std::string compiler_id() {
#ifdef _MSC_VER
    return "MSVC " + std::to_string(_MSC_VER);
#else
    return "unknown";
#endif
}

std::string utc_now() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return b;
}

const char* ftype_name(std::uint32_t t) {
    switch (t) {
        case 0:
            return "F32";
        case 1:
            return "F16";
        case 2:
            return "Q4_0";
        case 3:
            return "Q4_1";
        case 7:
            return "Q8_0";
        case 8:
            return "Q5_0";
        case 9:
            return "Q5_1";
        case 10:
            return "Q2_K";
        case 11:
            return "Q3_K_S";
        case 12:
            return "Q3_K_M";
        case 13:
            return "Q3_K_L";
        case 14:
            return "Q4_K_S";
        case 15:
            return "Q4_K_M";
        case 16:
            return "Q5_K_S";
        case 17:
            return "Q5_K_M";
        case 18:
            return "Q6_K";
        case 32:
            return "BF16";
        default:
            return "unknown";
    }
}

// Device-level sequential read speed bypassing the Windows file cache (cold-equivalent ceiling).
// FILE_FLAG_NO_BUFFERING needs sector-aligned buffers/sizes; the final short read at EOF is expected.
double unbuffered_sequential_mib_s(const fs::path& p, std::uint64_t limit_bytes, std::string& err) {
#ifdef _WIN32
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = "open failed (Win32 " + std::to_string(GetLastError()) + ")";
        return 0;
    }
    constexpr DWORD kBlock = 4u << 20;
    void* buf = VirtualAlloc(nullptr, kBlock, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    std::uint64_t total = 0;
    const auto t0 = Clock::now();
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf, kBlock, &got, nullptr)) {
            err = "read failed (Win32 " + std::to_string(GetLastError()) + ")";
            break;
        }
        if (got == 0)
            break;
        total += got;
        if (limit_bytes && total >= limit_bytes)
            break;
    }
    const double s = secs(t0, Clock::now());
    VirtualFree(buf, 0, MEM_RELEASE);
    CloseHandle(h);
    return s > 0 ? (static_cast<double>(total) / kMiB) / s : 0;
#else
    (void)p;
    (void)limit_bytes;
    err = "unsupported platform";
    return 0;
#endif
}

struct Args {
    fs::path source, scratch, out;
    std::string suite = "storage";
    std::string codecs = "raw,lz4,zstd";
    std::string git_commit = "unknown";
    std::string source_label = "unknown", scratch_label = "unknown";
    std::uint32_t chunk_mib = 16;
    int iterations = 3, warmup = 1, zstd_level = 3;
    std::size_t buffers = 4, workers = 0;
    bool keep = false, unbuffered = true;
    bool ram_sink = true;
};

void usage() {
    std::cerr
        << "hcs_storage_bench --suite storage --source <model.gguf> --scratch <dir> [options]\n"
           "  --source <file>        model to benchmark (read-only)\n"
           "  --scratch <dir>        existing directory where NEW .hcsmodel files are created and deleted\n"
           "  --out <file.json>      write benchmark JSON (schema_version 1); also printed to stdout\n"
           "  --codecs raw,lz4,zstd  codecs to compare (default all)\n"
           "  --chunk-mib N          chunk size, default 16\n"
           "  --iterations N         measured iterations (default 3), --warmup N (default 1)\n"
           "  --buffers N --workers N  pipeline slots / decode threads\n"
           "  --zstd-level N         default 3\n"
           "  --no-unbuffered        skip the NO_BUFFERING device-speed probes\n"
           "  --discard-sink         do not copy decoded data into a RAM buffer\n"
           "  --keep                 keep the created containers\n"
           "  --git-commit S --source-label S --scratch-label S   metadata strings\n";
}

}  // namespace

int main(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << k << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (k == "--suite")
            a.suite = next();
        else if (k == "--source")
            a.source = next();
        else if (k == "--scratch")
            a.scratch = next();
        else if (k == "--out")
            a.out = next();
        else if (k == "--codecs")
            a.codecs = next();
        else if (k == "--chunk-mib")
            a.chunk_mib = static_cast<std::uint32_t>(std::stoul(next()));
        else if (k == "--iterations")
            a.iterations = std::stoi(next());
        else if (k == "--warmup")
            a.warmup = std::stoi(next());
        else if (k == "--buffers")
            a.buffers = std::stoul(next());
        else if (k == "--workers")
            a.workers = std::stoul(next());
        else if (k == "--zstd-level")
            a.zstd_level = std::stoi(next());
        else if (k == "--git-commit")
            a.git_commit = next();
        else if (k == "--source-label")
            a.source_label = next();
        else if (k == "--scratch-label")
            a.scratch_label = next();
        else if (k == "--keep")
            a.keep = true;
        else if (k == "--no-unbuffered")
            a.unbuffered = false;
        else if (k == "--discard-sink")
            a.ram_sink = false;
        else if (k == "--help" || k == "-h") {
            usage();
            return 0;
        } else {
            std::cerr << "unknown argument " << k << "\n";
            usage();
            return 2;
        }
    }
    if (a.suite != "storage" || a.source.empty() || a.scratch.empty() || a.iterations < 1 || a.warmup < 0 ||
        a.chunk_mib < 1) {
        usage();
        return 2;
    }
    std::error_code ec;
    if (!fs::is_regular_file(a.source, ec)) {
        std::cerr << "source is not a regular file\n";
        return 7;
    }
    if (!fs::is_directory(a.scratch, ec)) {
        std::cerr << "scratch directory does not exist (it is never created implicitly)\n";
        return 7;
    }
    const std::uint32_t chunk_size = a.chunk_mib << 20;

    auto reader = PlainFileReader::open(a.source, chunk_size);
    if (!reader) {
        std::cerr << "cannot open source: " << reader.error().message << "\n";
        return reader.error().exit_code();
    }
    const std::uint64_t src_size = reader.value()->logical_size();

    json root;
    root["schema_version"] = 1;
    root["suite"] = "storage";
    root["timestamp_utc"] = utc_now();
    root["git_commit"] = a.git_commit;
#ifdef NDEBUG
    root["build_type"] = "Release";
#else
    root["build_type"] = "Debug";
#endif
    root["compiler"] = compiler_id();
    root["cmake_version"] = HCS_CMAKE_VERSION;
    root["os_build"] = os_build();
    root["cpu"] = cpu_brand();
    root["gpu"] = "n/a (storage benchmark, no GPU used)";
    root["gpu_vendor_id"] = "n/a";
    root["gpu_device_id"] = "n/a";
    root["driver_version"] = "n/a";
    root["vulkan_api_version"] = "n/a";
    root["ram_bytes"] = total_ram();
    root["model_format"] = reader.value()->gguf() ? "GGUF" : "raw-file";
    root["model_file"] = a.source.filename().string();
    root["model_size_bytes"] = src_size;
    const auto& gi = reader.value()->gguf();
    root["quantization"] = (gi && gi->file_type) ? ftype_name(*gi->file_type) : "unknown";
    root["gguf_architecture"] = gi ? gi->architecture : "unknown";
    root["gguf_tensor_count"] = gi ? json(gi->tensor_count) : json("unknown");
    root["context_tokens"] = nullptr;
    root["backend"] = "CPU";
    root["storage_source"] = a.source_label;
    root["scratch_storage"] = a.scratch_label;
    root["warmup_iterations"] = a.warmup;
    root["measured_iterations"] = a.iterations;
    root["chunk_bytes"] = chunk_size;
    root["pipeline_buffers"] = a.buffers;
    root["pipeline_workers"] = a.workers ? json(a.workers) : json("auto");
    root["sha256_implementation"] = sha256_implementation();
    root["lz4_version"] = std::string(lz4_version());
    root["zstd_version"] = std::string(zstd_version());
    root["sink"] = a.ram_sink ? "ram-copy" : "discard";
    root["cold_or_warm"] =
        "uncontrolled (Windows file cache not flushed; see unbuffered_* fields for device speed)";
    root["ttft_ms_median"] = nullptr;
    root["prompt_tokens_per_second_median"] = nullptr;
    root["decode_tokens_per_second_median"] = nullptr;
    root["decompression_mib_s_median"] = nullptr;  // per-codec values are under "runs"

    // ---- source baseline 1: first-touch read + SHA-256 of the whole source (also the manifest hash).
    std::string source_sha;
    {
        Sha256 h;
        StreamOptions so;
        so.buffers = a.buffers;
        so.workers = a.workers;
        auto st = reader.value()->stream(
            [&](std::size_t, const ChunkEntry&, std::span<const std::uint8_t> d) -> hcs::Result<void> {
                h.update(d);
                return {};
            },
            so);
        if (!st) {
            std::cerr << "source read failed: " << st.error().message << "\n";
            return st.error().exit_code();
        }
        source_sha = to_hex(h.finalize());
        root["model_sha256"] = source_sha;
        root["source_first_pass"] = {
            {"note", "first pass in this process; includes SHA-256; cache state uncontrolled"},
            {"seconds", st.value().wall_seconds},
            {"mib_s", (static_cast<double>(src_size) / kMiB) / st.value().wall_seconds}};
    }
    // ---- source baseline 2: device speed with the file cache bypassed (cold-equivalent ceiling).
    if (a.unbuffered) {
        std::string err;
        const double v = unbuffered_sequential_mib_s(a.source, 0, err);
        root["source_unbuffered_seq_mib_s"] = err.empty() ? json(v) : json("unknown: " + err);
        root["hdd_read_mib_s_median"] = err.empty() ? json(v) : json("unknown");
    } else {
        root["hdd_read_mib_s_median"] = "unknown";
    }
    // ---- source baseline 3: pipeline stream of the plain file (raw GGUF through the same machinery).
    std::vector<std::byte> dest;
    if (a.ram_sink)
        dest.resize(static_cast<std::size_t>(src_size));  // the "model resident in RAM" target
    auto run_stream = [&](const IModelReader& r, std::uint64_t& raw_out,
                          StreamStats& stats_out) -> hcs::Result<double> {
        StreamOptions so;
        so.buffers = a.buffers;
        so.workers = a.workers;
        std::uint64_t cursor = 0;
        auto t0 = Clock::now();
        auto st = r.stream(
            [&](std::size_t, const ChunkEntry&, std::span<const std::uint8_t> d) -> hcs::Result<void> {
                if (a.ram_sink)
                    std::memcpy(dest.data() + cursor, d.data(), d.size());
                cursor += d.size();
                return {};
            },
            so);
        if (!st)
            return st.error();
        raw_out = cursor;
        stats_out = st.value();
        return secs(t0, Clock::now());
    };
    {
        std::vector<double> t;
        StreamStats last;
        for (int it = 0; it < a.warmup + a.iterations; ++it) {
            std::uint64_t raw = 0;
            auto s = run_stream(*reader.value(), raw, last);
            if (!s) {
                std::cerr << "baseline stream failed: " << s.error().message << "\n";
                return s.error().exit_code();
            }
            if (it >= a.warmup)
                t.push_back(s.value());
        }
        std::vector<double> mibs;
        for (double s : t) mibs.push_back((static_cast<double>(src_size) / kMiB) / s);
        root["source_plain_stream"] = {{"seconds_samples", t},
                                       {"seconds_median", median(t)},
                                       {"seconds_p90", p90(t)},
                                       {"mib_s_median", median(mibs)},
                                       {"buffer_bytes_allocated", last.buffer_bytes_allocated},
                                       {"consumer_wait_seconds_last", last.consumer_wait_seconds}};
    }
    // SHA-256 throughput on a RAM-resident buffer (isolates hashing cost from I/O).
    {
        std::vector<std::uint8_t> buf(chunk_size, 0x5A);
        auto bench = [&](bool portable) {
            sha256_force_portable(portable);
            std::vector<double> v;
            for (int i = 0; i < 5; ++i) {
                const auto t0 = Clock::now();
                volatile std::uint8_t sink = sha256(buf)[0];
                (void)sink;
                v.push_back((static_cast<double>(buf.size()) / kMiB) / secs(t0, Clock::now()));
            }
            return median(v);
        };
        const double hw = bench(false);
        const double sw = bench(true);
        sha256_force_portable(false);
        root["sha256_mib_s"] = {
            {"selected", hw}, {"portable", sw}, {"selected_impl", sha256_implementation()}};
    }

    // ---- per-codec runs
    std::vector<std::string> codec_names;
    {
        std::string cur;
        for (char c : a.codecs + ",") {
            if (c == ',') {
                if (!cur.empty())
                    codec_names.push_back(cur);
                cur.clear();
            } else
                cur.push_back(c);
        }
    }
    json runs = json::array();
    std::vector<fs::path> created;
    int rc = 0;
    const unsigned long pid =
#ifdef _WIN32
        GetCurrentProcessId();
#else
        0;
#endif
    for (const auto& cn : codec_names) {
        const auto codec = parse_codec(cn);
        if (!codec) {
            std::cerr << "unknown codec " << cn << "\n";
            rc = 2;
            break;
        }
        const fs::path dst = a.scratch / ("hcs_bench_" + std::to_string(pid) + "_" + cn + ".hcsmodel");
        WriteOptions wo;
        wo.codec = *codec;
        wo.level = (*codec == Codec::Zstd) ? a.zstd_level : 0;
        wo.chunk_size = chunk_size;
        wo.model_id = a.source.stem().string().substr(0, 63);
        wo.architecture = gi ? gi->architecture.substr(0, 31) : "";
        wo.quantization = (gi && gi->file_type) ? ftype_name(*gi->file_type) : "";
        auto w = write_hcsmodel(a.source, dst, wo);
        if (!w) {
            std::cerr << "write(" << cn << ") failed: " << w.error().message << "\n";
            rc = w.error().exit_code();
            break;
        }
        created.push_back(dst);
        const WriteStats ws = w.value();
        json run;
        run["codec"] = cn;
        run["codec_level"] = (*codec == Codec::Zstd) ? json(a.zstd_level) : json("default");
        run["write"] = {{"seconds", ws.seconds},
                        {"compress_cpu_seconds", ws.compress_cpu_seconds},
                        {"source_read_seconds", ws.source_read_seconds},
                        {"container_bytes", ws.container_bytes},
                        {"payload_bytes", ws.payload_bytes},
                        {"ratio_container_over_raw", ws.ratio()},
                        {"chunks", ws.chunks},
                        {"chunks_raw", ws.chunks_raw},
                        {"chunks_lz4", ws.chunks_lz4},
                        {"chunks_zstd", ws.chunks_zstd},
                        {"min_savings_threshold", wo.min_savings}};
        run["manifest_matches_source"] = (ws.source_sha256_hex == source_sha);

        auto cr = HcsModelReader::open(dst);
        if (!cr) {
            std::cerr << "reopen failed: " << cr.error().message << "\n";
            rc = cr.error().exit_code();
            break;
        }

        // bit-exactness: stream and hash the decoded data, compare with the source hash.
        {
            Sha256 h;
            StreamOptions so;
            so.buffers = a.buffers;
            so.workers = a.workers;
            auto st = cr.value()->stream(
                [&](std::size_t, const ChunkEntry&, std::span<const std::uint8_t> d) -> hcs::Result<void> {
                    h.update(d);
                    return {};
                },
                so);
            if (!st) {
                std::cerr << "verify stream failed: " << st.error().message << "\n";
                rc = st.error().exit_code();
                break;
            }
            run["bit_exact_vs_source"] = (to_hex(h.finalize()) == source_sha);
        }

        if (a.unbuffered) {
            std::string err;
            const double v = unbuffered_sequential_mib_s(dst, 0, err);
            run["scratch_unbuffered_seq_mib_s"] = err.empty() ? json(v) : json("unknown: " + err);
        }

        // pipeline load (read + decode + verify + copy to RAM), repeated
        std::vector<double> t, wait;
        StreamStats last;
        for (int it = 0; it < a.warmup + a.iterations; ++it) {
            std::uint64_t raw = 0;
            auto s = run_stream(*cr.value(), raw, last);
            if (!s) {
                std::cerr << "stream failed: " << s.error().message << "\n";
                rc = s.error().exit_code();
                break;
            }
            if (raw != src_size) {
                std::cerr << "size mismatch\n";
                rc = 70;
                break;
            }
            if (it >= a.warmup) {
                t.push_back(s.value());
                wait.push_back(last.consumer_wait_seconds);
            }
        }
        if (rc)
            break;
        std::vector<double> logical_mibs, stored_mibs;
        for (double s : t) {
            logical_mibs.push_back((static_cast<double>(src_size) / kMiB) / s);
            stored_mibs.push_back((static_cast<double>(ws.payload_bytes) / kMiB) / s);
        }
        run["pipeline_load"] = {{"seconds_samples", t},
                                {"seconds_median", median(t)},
                                {"seconds_p90", p90(t)},
                                {"logical_mib_s_median", median(logical_mibs)},
                                {"stored_mib_s_median", median(stored_mibs)},
                                {"consumer_wait_seconds_median", median(wait)},
                                {"buffer_bytes_allocated", last.buffer_bytes_allocated},
                                {"cache_state", "warm/uncontrolled (container was just written)"}};
        run["hdd_read_mib_s_median"] = run["pipeline_load"]["stored_mib_s_median"];

        // decode-only from RAM, single thread: isolates codec + hash cost from I/O.
        {
            std::vector<std::vector<std::uint8_t>> stored(cr.value()->chunks().size());
            for (std::size_t i = 0; i < stored.size(); ++i) {
                const auto& e = cr.value()->chunks()[i];
                stored[i].resize(static_cast<std::size_t>(e.comp_size));
                if (auto r = cr.value()->file().read_at(e.offset, stored[i].data(), stored[i].size()); !r) {
                    rc = r.error().exit_code();
                    break;
                }
            }
            if (rc)
                break;
            std::vector<std::uint8_t> raw(chunk_size);
            auto decode_all = [&](bool verify) {
                const auto t0 = Clock::now();
                for (std::size_t i = 0; i < stored.size(); ++i) {
                    const auto& e = cr.value()->chunks()[i];
                    auto r = decode_chunk(
                        e, stored[i],
                        std::span<std::uint8_t>(raw.data(), static_cast<std::size_t>(e.raw_size)), verify);
                    if (!r) {
                        std::cerr << "decode failed: " << r.error().message << "\n";
                        std::exit(70);
                    }
                }
                return secs(t0, Clock::now());
            };
            std::vector<double> nv, wv;
            decode_all(false);  // warm-up
            for (int it = 0; it < a.iterations; ++it) {
                nv.push_back((static_cast<double>(src_size) / kMiB) / decode_all(false));
                wv.push_back((static_cast<double>(src_size) / kMiB) / decode_all(true));
            }
            run["decode_from_ram_single_thread"] = {{"codec_only_mib_s_median", median(nv)},
                                                    {"codec_plus_sha256_mib_s_median", median(wv)}};
            run["decompression_mib_s_median"] = median(nv);
        }
        run["peak_ram_bytes"] = peak_working_set();
        run["peak_ram_note"] =
            "process-lifetime peak working set after this codec; includes the benchmark's own RAM "
            "destination buffer";
        run["result_status"] = "PASS";
        runs.push_back(run);
    }
    root["runs"] = runs;
    root["peak_ram_bytes"] = peak_working_set();
    root["result_status"] = (rc == 0) ? "PASS" : "FAIL";

    // Delete ONLY the files this process created.
    if (!a.keep) {
        for (const auto& p : created) {
            std::error_code e;
            fs::remove(p, e);
        }
    }
    root["created_files_removed"] = !a.keep;

    const std::string text = root.dump(2);
    std::cout << text << "\n";
    if (!a.out.empty()) {
        std::FILE* f = nullptr;
#ifdef _WIN32
        f = _wfopen(a.out.c_str(), L"wb");
#else
        f = std::fopen(a.out.c_str(), "wb");
#endif
        if (f) {
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
        } else {
            std::cerr << "cannot write --out file\n";
            return 8;
        }
    }
    return rc;
}
