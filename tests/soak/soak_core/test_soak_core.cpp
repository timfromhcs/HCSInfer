// Reliability soak over the REAL stack (api::Runtime = what `HCSInfer serve` builds): real llama.cpp
// text/vision and stable-diffusion.cpp engines, real models. Evidence is appended as JSON lines to
// $HCS_SOAK_OUT/soak-core.jsonl. See docs/soak.md for thresholds and knobs.
//
// Env: HCS_SOAK_MODEL_ROOT (required, else NOT RUN), HCS_SOAK_OUT, HCS_SOAK_TEXT_ID / HCS_SOAK_IMAGE_ID,
//      HCS_SOAK_CYCLES, HCS_SOAK_SWITCH_CYCLES, HCS_SOAK_GEN_SECONDS, HCS_SOAK_VISION_RUNS,
//      HCS_SOAK_IMAGE_RUNS, HCS_SOAK_CANCEL_ROUNDS, HCS_SOAK_SHUTDOWN_ROUNDS, HCS_SOAK_UNPLUG_ROUNDS,
//      HCS_SOAK_MAX_PRIVATE_GROWTH_MB, HCS_SOAK_MAX_HANDLE_GROWTH, HCS_SOAK_MAX_THREAD_GROWTH,
//      HCS_SOAK_MAX_TPS_DROP_PCT.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <nlohmann/json.hpp>
#include <numeric>
#include <sstream>
#include <thread>

#include "hcs/api/runtime.hpp"
#include "hcs/storage/file_source.hpp"
#include "hcs/storage/gguf.hpp"
#include "hcs/storage/model_reader.hpp"
#include "hcs/storage/writer.hpp"
#include "temp_dir.hpp"
#include "test_png.hpp"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

using namespace hcs;
using namespace hcs::api;
using namespace std::chrono_literals;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string env(const char* n, const std::string& def = "") {
    const char* v = std::getenv(n);
    return (v && *v) ? v : def;
}
long long env_i(const char* n, long long def) {
    const auto s = env(n);
    if (s.empty())
        return def;
    try {
        return std::stoll(s);
    } catch (...) {
        return def;
    }
}

#define REQUIRE_ROOT_OR_SKIP()              \
    if (env("HCS_SOAK_MODEL_ROOT").empty()) \
    GTEST_SKIP() << "NOT RUN: HCS_SOAK_MODEL_ROOT is not set"

// ---- process metrics ----------------------------------------------------------------------------------
struct Proc {
    std::uint64_t private_bytes = 0;  // commit charge of this process (mapped model files are NOT included)
    std::uint64_t working_set = 0;
    std::uint32_t handles = 0;
    std::uint32_t threads = 0;
};

Proc sample() {
    Proc p;
    PROCESS_MEMORY_COUNTERS_EX c{};
    c.cb = sizeof(c);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&c),
                             sizeof(c))) {
        p.private_bytes = c.PrivateUsage;
        p.working_set = c.WorkingSetSize;
    }
    DWORD h = 0;
    GetProcessHandleCount(GetCurrentProcess(), &h);
    p.handles = h;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        const DWORD me = GetCurrentProcessId();
        if (Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID == me)
                    ++p.threads;
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
    }
    return p;
}

json proc_json(const Proc& p) {
    return {{"private_bytes", p.private_bytes},
            {"working_set", p.working_set},
            {"handles", p.handles},
            {"threads", p.threads}};
}

// ---- evidence sink --------------------------------------------------------------------------------------
std::mutex g_out_mu;
void emit(json j) {
    std::lock_guard lk(g_out_mu);
    const fs::path dir = env("HCS_SOAK_OUT", (fs::temp_directory_path() / "hcs-soak").string());
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream f(dir / "soak-core.jsonl", std::ios::app);
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    j["t_unix_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    f << j.dump() << "\n";
}

// ---- thresholds ------------------------------------------------------------------------------------------
struct Limits {
    double private_mb = static_cast<double>(env_i("HCS_SOAK_MAX_PRIVATE_GROWTH_MB", 128));
    long long handles = env_i("HCS_SOAK_MAX_HANDLE_GROWTH", 32);
    long long threads = env_i("HCS_SOAK_MAX_THREAD_GROWTH", 4);
};

// Leak verdict: compare the first sample after warm-up with the last sample. Growth beyond the documented
// threshold (docs/soak.md) fails the test.
void check_growth(const std::string& test, const std::vector<Proc>& s, std::size_t warmup) {
    ASSERT_GE(s.size(), 2u) << "not enough samples for a leak verdict";
    warmup = std::min(warmup, s.size() - 2);  // short runs: compare first vs last sample
    const Proc& a = s[warmup];
    const Proc& b = s.back();
    const Limits lim;
    const double dpriv =
        (static_cast<double>(b.private_bytes) - static_cast<double>(a.private_bytes)) / 1048576.0;
    const long long dh = static_cast<long long>(b.handles) - static_cast<long long>(a.handles);
    const long long dt = static_cast<long long>(b.threads) - static_cast<long long>(a.threads);
    const double n = static_cast<double>(s.size() - warmup);
    double sx = 0, sy = 0, sxy = 0, sxx = 0;
    for (std::size_t i = warmup; i < s.size(); ++i) {
        const double x = static_cast<double>(i - warmup);
        const double y = static_cast<double>(s[i].private_bytes) / 1048576.0;
        sx += x, sy += y, sxy += x * y, sxx += x * x;
    }
    const double den = n * sxx - sx * sx;
    const double slope = den != 0 ? (n * sxy - sx * sy) / den : 0.0;
    const bool pass = dpriv <= lim.private_mb && dh <= lim.handles && dt <= lim.threads;
    emit({{"test", test},
          {"kind", "leak_verdict"},
          {"samples", s.size()},
          {"warmup_samples", warmup},
          {"private_growth_mb", dpriv},
          {"private_slope_mb_per_sample", slope},
          {"handle_growth", dh},
          {"thread_growth", dt},
          {"limits", {{"private_mb", lim.private_mb}, {"handles", lim.handles}, {"threads", lim.threads}}},
          {"first", proc_json(a)},
          {"last", proc_json(b)},
          {"verdict", pass ? "PASS" : "FAIL"}});
    EXPECT_LE(dpriv, lim.private_mb) << test << ": private-bytes growth (leak)";
    EXPECT_LE(dh, lim.handles) << test << ": handle growth (leak)";
    EXPECT_LE(dt, lim.threads) << test << ": thread growth (leak)";
}

// ---- rig
// ---------------------------------------------------------------------------------------------------
struct Rig {
    test::TempDir state;
    std::ostringstream sink;
    Logger logger{&sink, LogLevel::Warn};
    std::unique_ptr<Runtime> rt;
    std::string text_id, image_id;

    explicit Rig(const std::vector<std::string>& more_roots = {}) {
        RuntimeOptions o;
        o.state_dir = state.path();
        o.model_roots.push_back(env("HCS_SOAK_MODEL_ROOT"));
        for (const auto& r : more_roots) o.model_roots.push_back(r);
        auto r = Runtime::create(std::move(o), logger);
        EXPECT_TRUE(r.ok()) << (r.ok() ? "" : r.error().message);
        if (!r.ok())
            return;
        rt = std::move(r.value());
        (void)rt->registry.scan(fs::path(env("HCS_SOAK_MODEL_ROOT")), rt->guard);
        text_id = find_id(env("HCS_SOAK_TEXT_ID", "lfm3-omni-2b-q8_0"), "mmproj");
        image_id = find_id(env("HCS_SOAK_IMAGE_ID", "sd15-q8_0"), "mmproj");
    }
    std::string find_id(const std::string& needle, const std::string& exclude) const {
        for (const auto& m : rt->registry.list()) {
            if (m.id.find(needle) != std::string::npos && m.id.find(exclude) == std::string::npos)
                return m.id;
        }
        return {};
    }
    ControlPlane& cp() { return *rt->control; }
};

struct Outcome {
    JobState state = JobState::Failed;
    std::string text;
    std::string code;
    bool timed_out = false;
};

Outcome drain(ControlPlane& cp, const Submitted& s, std::chrono::seconds timeout) {
    Outcome o;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        JobEvent ev;
        const auto r = s.channel->pop(ev, deadline);
        if (r == JobChannel::Pop::Event) {
            if (ev.kind == JobEventKind::Output)
                o.text += ev.text;
            continue;
        }
        if (r == JobChannel::Pop::Timeout) {
            o.timed_out = true;
            return o;
        }
        break;
    }
    (void)cp.wait_job(s.id, 30s);
    auto st = cp.job_status(s.id);
    if (st.ok()) {
        o.state = st.value().state;
        if (st.value().error)
            o.code = std::string(to_string(st.value().error->code));
    }
    return o;
}

ChatParams text_params(const std::string& id, std::uint32_t max_tokens, std::uint64_t seed = 42) {
    ChatParams p;
    p.model = id;
    p.messages.push_back({"user", "Write a long story about a lighthouse keeper and a storm.", {}});
    p.max_tokens = max_tokens;
    p.temperature = 0.7f;
    p.seed = seed;
    return p;
}

double median(std::vector<double> v) {
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

}  // namespace

// ---- 1. 100x load/unload ----------------------------------------------------------------------------------
TEST(Soak, LoadUnloadCycles) {
    REQUIRE_ROOT_OR_SKIP();
    Rig rig;
    ASSERT_TRUE(rig.rt);
    ASSERT_FALSE(rig.text_id.empty()) << "text model not found under HCS_SOAK_MODEL_ROOT";
    const long long cycles = env_i("HCS_SOAK_CYCLES", 5);
    std::vector<Proc> samples;
    std::vector<double> load_ms, unload_ms;
    for (long long i = 0; i < cycles; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        auto l = rig.cp().load_model(rig.text_id);
        const auto t1 = std::chrono::steady_clock::now();
        ASSERT_TRUE(l.ok()) << "cycle " << i << ": " << l.error().message;
        std::string gen_note;
        if (i % 10 == 0) {  // liveness: a short real generation every 10th cycle
            auto s = rig.cp().submit_chat(text_params(rig.text_id, 8));
            ASSERT_TRUE(s.ok()) << s.error().message;
            auto o = drain(rig.cp(), s.value(), 120s);
            EXPECT_EQ(o.state, JobState::Succeeded) << "cycle " << i;
            EXPECT_FALSE(o.text.empty()) << "cycle " << i;
            gen_note = "generated";
        }
        const auto t1b = std::chrono::steady_clock::now();
        auto u = rig.cp().unload_model();
        const auto t2 = std::chrono::steady_clock::now();
        ASSERT_TRUE(u.ok()) << "cycle " << i << ": " << u.error().message;
        EXPECT_FALSE(rig.cp().loaded_model().has_value());
        const Proc p = sample();
        samples.push_back(p);
        load_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        unload_ms.push_back(std::chrono::duration<double, std::milli>(t2 - t1b).count());
        emit({{"test", "LoadUnloadCycles"},
              {"kind", "sample"},
              {"cycle", i},
              {"load_ms", load_ms.back()},
              {"unload_ms", unload_ms.back()},
              {"note", gen_note},
              {"proc", proc_json(p)},
              {"budget_reserved", rig.rt->budget.reserved()}});
        EXPECT_EQ(rig.rt->budget.reserved(), 0u) << "memory budget not released after unload, cycle " << i;
    }
    emit({{"test", "LoadUnloadCycles"},
          {"kind", "summary"},
          {"cycles", cycles},
          {"load_ms_median", median(load_ms)},
          {"unload_ms_median", median(unload_ms)}});
    check_growth("LoadUnloadCycles", samples, std::min<std::size_t>(3, samples.size() / 2));
}

// ---- 2. text <-> image switching -----------------------------------------------------------------------
TEST(Soak, ModelSwitchCycles) {
    REQUIRE_ROOT_OR_SKIP();
    Rig rig;
    ASSERT_TRUE(rig.rt);
    if (rig.text_id.empty() || rig.image_id.empty())
        GTEST_SKIP() << "NOT RUN: needs both a text and an image model";
    const long long cycles = env_i("HCS_SOAK_SWITCH_CYCLES", 2);
    std::vector<Proc> samples;
    for (long long i = 0; i < cycles; ++i) {
        ASSERT_TRUE(rig.cp().load_model(rig.text_id).ok()) << i;
        ASSERT_TRUE(rig.cp().load_model(rig.image_id).ok()) << i;  // switch frees the text model
        EXPECT_EQ(rig.cp().loaded_model().value_or(""), rig.image_id);
        ASSERT_TRUE(rig.cp().unload_model().ok()) << i;
        samples.push_back(sample());
        emit({{"test", "ModelSwitchCycles"},
              {"kind", "sample"},
              {"cycle", i},
              {"proc", proc_json(samples.back())}});
    }
    check_growth("ModelSwitchCycles", samples, std::min<std::size_t>(2, samples.size() / 2));
}

// ---- 3. continuous generation (tokens/s drift + memory) -----------------------------------------------
TEST(Soak, ContinuousGeneration) {
    REQUIRE_ROOT_OR_SKIP();
    Rig rig;
    ASSERT_TRUE(rig.rt);
    ASSERT_FALSE(rig.text_id.empty());
    {
        auto lm = rig.cp().load_model(rig.text_id);
        ASSERT_TRUE(lm.ok()) << lm.error().message;
    }
    const auto seconds = std::chrono::seconds(env_i("HCS_SOAK_GEN_SECONDS", 20));
    const auto start = std::chrono::steady_clock::now();
    std::vector<Proc> samples;
    std::vector<double> tps, t_at;
    std::uint64_t total_tokens = 0, failures = 0;
    long long it = 0;
    while (std::chrono::steady_clock::now() - start < seconds) {
        auto s = rig.cp().submit_chat(text_params(rig.text_id, 128, 1000 + static_cast<std::uint64_t>(it)));
        ASSERT_TRUE(s.ok()) << s.error().message;
        auto o = drain(rig.cp(), s.value(), 300s);
        ASSERT_FALSE(o.timed_out) << "generation hung at iteration " << it;
        auto r = rig.cp().chat_result(s.value().id);
        if (o.state != JobState::Succeeded || !r.ok() || o.text.empty()) {
            ++failures;
            emit({{"test", "ContinuousGeneration"}, {"kind", "failure"}, {"iter", it}, {"code", o.code}});
        } else {
            tps.push_back(r.value().tokens_per_second);
            total_tokens += r.value().completion_tokens;
        }
        const Proc p = sample();
        samples.push_back(p);
        t_at.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        emit({{"test", "ContinuousGeneration"},
              {"kind", "sample"},
              {"iter", it},
              {"t_s", t_at.back()},
              {"tokens_per_second", r.ok() ? r.value().tokens_per_second : 0.0},
              {"completion_tokens", r.ok() ? r.value().completion_tokens : 0u},
              {"ttft_ms", r.ok() ? r.value().ttft_ms : 0.0},
              {"proc", proc_json(p)}});
        ++it;
    }
    EXPECT_EQ(failures, 0u);
    ASSERT_GE(tps.size(), 4u) << "too few successful iterations for a drift verdict";
    const std::size_t k = std::max<std::size_t>(1, tps.size() / 5);
    const double first = median(std::vector<double>(tps.begin(), tps.begin() + static_cast<long>(k)));
    const double last = median(std::vector<double>(tps.end() - static_cast<long>(k), tps.end()));
    const double drift_pct = first > 0 ? (last - first) / first * 100.0 : 0.0;
    const double max_drop = static_cast<double>(env_i("HCS_SOAK_MAX_TPS_DROP_PCT", 25));
    emit({{"test", "ContinuousGeneration"},
          {"kind", "summary"},
          {"iterations", it},
          {"duration_s", t_at.empty() ? 0.0 : t_at.back()},
          {"total_completion_tokens", total_tokens},
          {"tps_first_fifth_median", first},
          {"tps_last_fifth_median", last},
          {"tps_drift_pct", drift_pct},
          {"tps_drop_limit_pct", max_drop},
          {"backend", rig.rt->engine->metrics().backend},
          {"device", rig.rt->engine->metrics().device}});
    EXPECT_GE(drift_pct, -max_drop) << "decode tokens/s degraded by more than the limit over the run";
    check_growth("ContinuousGeneration", samples, std::min<std::size_t>(3, samples.size() / 2));
}

// ---- 4. repeated vision ----------------------------------------------------------------------------------
TEST(Soak, VisionRepeat) {
    REQUIRE_ROOT_OR_SKIP();
    Rig rig;
    ASSERT_TRUE(rig.rt);
    ASSERT_FALSE(rig.text_id.empty());
    auto m = rig.cp().model(rig.text_id);
    ASSERT_TRUE(m.ok());
    auto proj = rig.cp().projector_path_for(m.value().manifest);
    if (!proj.ok() || proj.value().empty())
        GTEST_SKIP() << "NOT RUN: no mmproj paired with " << rig.text_id;
    const long long runs = env_i("HCS_SOAK_VISION_RUNS", 3);
    std::vector<Proc> samples;
    for (long long i = 0; i < runs; ++i) {
        ChatParams p;
        p.model = rig.text_id;
        ImageInput img;
        img.bytes = test::make_red_square_png(static_cast<std::uint32_t>(64 + (i % 4) * 16));
        p.messages.push_back({"user", "Describe this image in one short sentence.", {img}});
        p.max_tokens = 48;
        p.temperature = 0.0f;
        p.seed = 7;
        auto s = rig.cp().submit_chat(p);
        ASSERT_TRUE(s.ok()) << s.error().message;
        auto o = drain(rig.cp(), s.value(), 300s);
        EXPECT_FALSE(o.timed_out) << i;
        EXPECT_EQ(o.state, JobState::Succeeded) << "run " << i << " " << o.code;
        EXPECT_FALSE(o.text.empty()) << "run " << i;
        samples.push_back(sample());
        emit({{"test", "VisionRepeat"},
              {"kind", "sample"},
              {"run", i},
              {"chars", o.text.size()},
              {"proc", proc_json(samples.back())}});
    }
    check_growth("VisionRepeat", samples, std::min<std::size_t>(2, samples.size() / 2));
}

// ---- 5. repeated diffusion -------------------------------------------------------------------------------
TEST(Soak, DiffusionRepeat) {
    REQUIRE_ROOT_OR_SKIP();
    Rig rig;
    ASSERT_TRUE(rig.rt);
    if (rig.image_id.empty())
        GTEST_SKIP() << "NOT RUN: image model not found";
    ASSERT_TRUE(rig.cp().has_image_engine()) << "no diffusion engine in this build";
    const long long runs = env_i("HCS_SOAK_IMAGE_RUNS", 2);
    std::vector<Proc> samples;
    std::vector<double> ms;
    for (long long i = 0; i < runs; ++i) {
        ImageParams p;
        p.model = rig.image_id;
        p.prompt = "a simple red apple on a plain table";
        p.width = p.height = 256;
        p.allow_low_quality = true;  // 256 px soak/cancel workload, not a quality claim
        p.steps = 2;
        p.seed = 100 + static_cast<std::uint64_t>(i);
        auto s = rig.cp().submit_image(p, false);
        ASSERT_TRUE(s.ok()) << s.error().message;
        auto o = drain(rig.cp(), s.value(), 900s);
        ASSERT_FALSE(o.timed_out) << i;
        EXPECT_EQ(o.state, JobState::Succeeded) << "run " << i << " " << o.code;
        auto r = rig.cp().image_result(s.value().id);
        ASSERT_TRUE(r.ok()) << "run " << i;
        const auto& b = r.value().bytes;
        EXPECT_TRUE(b.size() > 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') << "not a PNG";
        EXPECT_EQ(r.value().width, 256u);
        ms.push_back(r.value().generation_ms);
        samples.push_back(sample());
        emit({{"test", "DiffusionRepeat"},
              {"kind", "sample"},
              {"run", i},
              {"png_bytes", b.size()},
              {"generation_ms", r.value().generation_ms},
              {"proc", proc_json(samples.back())}});
    }
    emit({{"test", "DiffusionRepeat"},
          {"kind", "summary"},
          {"runs", runs},
          {"generation_ms_median", median(ms)}});
    check_growth("DiffusionRepeat", samples, std::min<std::size_t>(2, samples.size() / 2));
}

// ---- 6. cancellation storm (in-process) -----------------------------------------------------------------
TEST(Soak, CancelStorm) {
    REQUIRE_ROOT_OR_SKIP();
    Rig rig;
    ASSERT_TRUE(rig.rt);
    ASSERT_FALSE(rig.text_id.empty());
    {
        auto lm = rig.cp().load_model(rig.text_id);
        ASSERT_TRUE(lm.ok()) << lm.error().message;
    }
    const long long rounds = env_i("HCS_SOAK_CANCEL_ROUNDS", 10);
    std::vector<Proc> samples;
    std::uint64_t cancelled = 0, succeeded = 0, other = 0;
    std::vector<double> cancel_latency_ms;
    for (long long i = 0; i < rounds; ++i) {
        // a burst of 3 jobs: the first runs, the others are queued; cancel all at a pseudo-random moment
        std::vector<Submitted> burst;
        for (int k = 0; k < 3; ++k) {
            auto s = rig.cp().submit_chat(
                text_params(rig.text_id, 512, 5000 + static_cast<std::uint64_t>(i * 3 + k)));
            if (s.ok())
                burst.push_back(s.value());
        }
        ASSERT_FALSE(burst.empty());
        std::this_thread::sleep_for(std::chrono::milliseconds((i * 37) % 400));
        const auto tc = std::chrono::steady_clock::now();
        for (const auto& b : burst) (void)rig.cp().cancel_job(b.id);
        for (const auto& b : burst) {
            auto o = drain(rig.cp(), b, 60s);
            EXPECT_FALSE(o.timed_out) << "job did not terminate after cancel, round " << i;
            if (o.state == JobState::Cancelled)
                ++cancelled;
            else if (o.state == JobState::Succeeded)
                ++succeeded;
            else {
                ++other;
                ADD_FAILURE() << "round " << i << ": unexpected terminal state " << to_string(o.state) << " "
                              << o.code;
            }
        }
        cancel_latency_ms.push_back(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tc).count());
        samples.push_back(sample());
        emit({{"test", "CancelStorm"},
              {"kind", "sample"},
              {"round", i},
              {"cancel_to_all_terminal_ms", cancel_latency_ms.back()},
              {"proc", proc_json(samples.back())}});
    }
    auto s = rig.cp().submit_chat(text_params(rig.text_id, 16));
    ASSERT_TRUE(s.ok());
    auto o = drain(rig.cp(), s.value(), 120s);
    EXPECT_EQ(o.state, JobState::Succeeded) << "engine unusable after cancel storm";
    EXPECT_FALSE(o.text.empty());
    emit({{"test", "CancelStorm"},
          {"kind", "summary"},
          {"rounds", rounds},
          {"cancelled", cancelled},
          {"succeeded_before_cancel", succeeded},
          {"other", other},
          {"cancel_to_terminal_ms_median", median(cancel_latency_ms)}});
    check_growth("CancelStorm", samples, std::min<std::size_t>(2, samples.size() / 2));
}

// ---- 7. shutdown while loading / generating / rendering ---------------------------------------------------
namespace {
// Destroys the rig on a helper thread with a watchdog: a hang is reported as a defect, not a stuck test.
double destroy_with_watchdog(std::unique_ptr<Rig>& rig, const char* scenario) {
    const auto t0 = std::chrono::steady_clock::now();
    // Direct destruction: ~Runtime() joins both job queues itself (defect D-S1, fixed; the contract test
    // RuntimeLifecycle.* in tests/contract/api covers it with a fake engine).
    auto fut = std::async(std::launch::async, [&] { rig.reset(); });
    if (fut.wait_for(180s) != std::future_status::ready) {
        emit({{"test", "Shutdown"}, {"scenario", scenario}, {"kind", "hang"}, {"verdict", "FAIL"}});
        ADD_FAILURE() << scenario << ": Runtime destruction did not finish within 180 s (hang)";
        std::fflush(nullptr);
        std::_Exit(3);
    }
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void wait_running(ControlPlane& cp, JobId id) {
    for (int i = 0; i < 600; ++i) {
        auto st = cp.job_status(id);
        if (st.ok() && st.value().state != JobState::Queued)
            return;
        std::this_thread::sleep_for(50ms);
    }
}
}  // namespace

TEST(Soak, ShutdownWhileGenerating) {
    REQUIRE_ROOT_OR_SKIP();
    const long long rounds = env_i("HCS_SOAK_SHUTDOWN_ROUNDS", 2);
    std::vector<Proc> samples;
    for (long long i = 0; i < rounds; ++i) {
        auto rig = std::make_unique<Rig>();
        ASSERT_TRUE(rig->rt);
        ASSERT_FALSE(rig->text_id.empty());
        {
            auto lm = rig->cp().load_model(rig->text_id);
            ASSERT_TRUE(lm.ok()) << lm.error().message;
        }
        auto s = rig->cp().submit_chat(text_params(rig->text_id, 2048, 9));
        ASSERT_TRUE(s.ok());
        wait_running(rig->cp(), s.value().id);
        std::this_thread::sleep_for(std::chrono::milliseconds(500 + (i * 311) % 1500));
        const double ms = destroy_with_watchdog(rig, "generating");
        samples.push_back(sample());
        emit({{"test", "ShutdownWhileGenerating"},
              {"kind", "sample"},
              {"round", i},
              {"destroy_ms", ms},
              {"proc", proc_json(samples.back())}});
        EXPECT_LT(ms, 60000.0) << "shutdown during generation took too long";
    }
    check_growth("ShutdownWhileGenerating", samples, std::min<std::size_t>(1, samples.size() / 2));
}

TEST(Soak, ShutdownWhileLoading) {
    REQUIRE_ROOT_OR_SKIP();
    const long long rounds = env_i("HCS_SOAK_SHUTDOWN_ROUNDS", 2);
    std::vector<Proc> samples;
    for (long long i = 0; i < rounds; ++i) {
        auto rig = std::make_unique<Rig>();
        ASSERT_TRUE(rig->rt);
        ASSERT_FALSE(rig->text_id.empty());
        auto j = rig->cp().submit_load(rig->text_id);
        ASSERT_TRUE(j.ok());
        std::this_thread::sleep_for(std::chrono::milliseconds(100 + (i * 173) % 600));  // mid-load
        const double ms = destroy_with_watchdog(rig, "loading");
        samples.push_back(sample());
        emit({{"test", "ShutdownWhileLoading"},
              {"kind", "sample"},
              {"round", i},
              {"destroy_ms", ms},
              {"proc", proc_json(samples.back())}});
        EXPECT_LT(ms, 120000.0);
    }
    check_growth("ShutdownWhileLoading", samples, std::min<std::size_t>(1, samples.size() / 2));
}

TEST(Soak, ShutdownWhileRenderingImage) {
    REQUIRE_ROOT_OR_SKIP();
    const long long rounds = env_i("HCS_SOAK_SHUTDOWN_ROUNDS", 2);
    std::vector<Proc> samples;
    for (long long i = 0; i < rounds; ++i) {
        auto rig = std::make_unique<Rig>();
        ASSERT_TRUE(rig->rt);
        if (rig->image_id.empty() || !rig->cp().has_image_engine())
            GTEST_SKIP() << "NOT RUN: no image model/engine";
        ImageParams p;
        p.model = rig->image_id;
        p.prompt = "a lighthouse in a storm";
        p.width = p.height = 256;
        p.allow_low_quality = true;  // 256 px soak/cancel workload, not a quality claim
        p.steps = 30;
        p.seed = 3;
        auto s = rig->cp().submit_image(p, false);
        ASSERT_TRUE(s.ok());
        wait_running(rig->cp(), s.value().id);
        std::this_thread::sleep_for(std::chrono::milliseconds(4000 + (i * 700) % 3000));
        const double ms = destroy_with_watchdog(rig, "rendering");
        samples.push_back(sample());
        emit({{"test", "ShutdownWhileRenderingImage"},
              {"kind", "sample"},
              {"round", i},
              {"destroy_ms", ms},
              {"proc", proc_json(samples.back())}});
        EXPECT_LT(ms, 120000.0);
    }
    check_growth("ShutdownWhileRenderingImage", samples, std::min<std::size_t>(1, samples.size() / 2));
}

// ---- 8. HDD-unplug simulation (fault injection; the real drive is never touched) ------------------------
namespace {
// A ByteSource that behaves like a USB drive that vanishes after `limit` bytes were served.
class UnpluggedSource final : public storage::ByteSource {
public:
    UnpluggedSource(const storage::ByteSource& inner, std::uint64_t limit) : inner_(inner), limit_(limit) {}
    std::uint64_t size() const noexcept override { return inner_.size(); }
    Result<void> read_at(std::uint64_t offset, void* dst, std::size_t n) const override {
        const auto served = served_.fetch_add(n);
        if (served + n > limit_)
            return Error(ErrorCode::IoError, "injected fault: device removed");
        return inner_.read_at(offset, dst, n);
    }

private:
    const storage::ByteSource& inner_;
    std::uint64_t limit_;
    mutable std::atomic<std::uint64_t> served_{0};
};

std::string text_gguf_path(Rig& rig) {
    auto m = rig.cp().model(rig.text_id);
    return m.ok() ? m.value().manifest.path : std::string{};
}
}  // namespace

TEST(Soak, UnplugGgufParseFaultInjection) {
    REQUIRE_ROOT_OR_SKIP();
    Rig rig;
    ASSERT_TRUE(rig.rt);
    ASSERT_FALSE(rig.text_id.empty());
    const auto path = text_gguf_path(rig);
    auto fsrc = storage::FileSource::open(path);
    ASSERT_TRUE(fsrc.ok()) << fsrc.error().message;
    ASSERT_TRUE(storage::parse_gguf(*fsrc.value()).ok()) << "unfaulted parse must succeed";
    const std::uint64_t limits[] = {0, 1, 8, 64, 4096, 65536, 1u << 20, 4u << 20};
    const long long rounds = env_i("HCS_SOAK_UNPLUG_ROUNDS", 5);
    std::vector<Proc> samples;
    for (long long r = 0; r < rounds; ++r) {
        for (auto lim : limits) {
            UnpluggedSource bad(*fsrc.value(), lim);
            Result<storage::GgufInfo> res = Error(ErrorCode::Internal, "unset");
            ASSERT_NO_THROW(res = storage::parse_gguf(bad));
            if (r == 0)
                emit({{"test", "UnplugGgufParse"},
                      {"kind", "case"},
                      {"fault_after_bytes", lim},
                      {"ok", res.ok()},
                      {"code", res.ok() ? "ok" : std::string(to_string(res.error().code))}});
            if (!res.ok())
                EXPECT_TRUE(res.error().code == ErrorCode::IoError ||
                            res.error().code == ErrorCode::ModelError)
                    << "unexpected error class: " << to_string(res.error().code);
            if (lim <= 8)
                EXPECT_FALSE(res.ok()) << "parse cannot succeed when the device vanished at byte " << lim;
        }
        samples.push_back(sample());
    }
    check_growth("UnplugGgufParse", samples, std::min<std::size_t>(1, samples.size() / 2));
}

TEST(Soak, UnplugTruncatedHcsModelStream) {
    // A container cut at every interesting offset == the drive vanished mid-copy. Never a crash/hang/partial
    // success.
    test::TempDir dir;
    const fs::path raw = dir / "raw.bin";
    {
        std::ofstream f(raw, std::ios::binary);
        std::uint64_t x = 88172645463325252ull;
        std::string buf(1u << 20, '\0');
        for (int mb = 0; mb < 6; ++mb) {
            for (std::size_t i = 0; i < buf.size(); ++i) {
                x ^= x << 13, x ^= x >> 7, x ^= x << 17;
                buf[i] = (i % 3 == 0) ? static_cast<char>(x) : static_cast<char>(i);
            }
            f.write(buf.data(), static_cast<std::streamsize>(buf.size()));
        }
    }
    const fs::path cont = dir / "full.hcsmodel";
    storage::WriteOptions wo;
    wo.codec = storage::Codec::Zstd;
    wo.chunk_size = 512u << 10;
    auto w = storage::write_hcsmodel(raw, cont, wo);
    ASSERT_TRUE(w.ok()) << w.error().message;
    const std::string bytes = test::read_file(cont);
    const auto full = static_cast<std::uint64_t>(bytes.size());
    ASSERT_GT(full, 1024u);

    auto stream_all = [](const fs::path& p, std::uint64_t& seen) {
        auto rd = storage::HcsModelReader::open(p);
        if (!rd.ok())
            return Result<void>(rd.error());
        auto st = rd.value()->stream(
            [&](std::size_t, const storage::ChunkEntry&, std::span<const std::uint8_t> d) -> Result<void> {
                seen += d.size();
                return {};
            },
            storage::StreamOptions{});
        if (!st.ok())
            return Result<void>(st.error());
        return Result<void>();
    };

    const long long rounds = env_i("HCS_SOAK_UNPLUG_ROUNDS", 5);
    std::vector<Proc> samples;
    for (long long r = 0; r < rounds; ++r) {
        int clean_errors = 0, cases = 0;
        const std::uint64_t cuts[] = {0, 7, 64, full / 8, full / 2, full - 17, full - 1};
        for (std::uint64_t cut : cuts) {
            const fs::path t = dir / ("cut-" + std::to_string(cut) + ".hcsmodel");
            {
                std::ofstream f(t, std::ios::binary | std::ios::trunc);
                f.write(bytes.data(), static_cast<std::streamsize>(cut));
            }
            ++cases;
            std::uint64_t seen = 0;
            auto res = stream_all(t, seen);
            EXPECT_FALSE(res.ok()) << "a container cut at " << cut << " of " << full
                                   << " streamed as success";
            if (!res.ok())
                ++clean_errors;
        }
        EXPECT_EQ(clean_errors, cases);
        samples.push_back(sample());
        std::uint64_t seen = 0;
        auto ok = stream_all(cont, seen);  // the intact container still streams in full afterwards
        EXPECT_TRUE(ok.ok());
        EXPECT_EQ(seen, w.value().raw_bytes);
        emit({{"test", "UnplugTruncatedHcsModel"},
              {"kind", "sample"},
              {"round", r},
              {"cases", cases},
              {"clean_errors", clean_errors},
              {"proc", proc_json(samples.back())}});
    }
    check_growth("UnplugTruncatedHcsModel", samples, std::min<std::size_t>(1, samples.size() / 2));
}

TEST(Soak, UnplugEngineLoadTruncatedAndVanishedFile) {
    // Engine-level: a model whose file is cut short (copy truncated at 32 MiB, in a private temp root) and a
    // model whose file disappears after registration. Both must fail the load cleanly, roll back the
    // lifecycle and budget, and leave the engine usable for the next (good) model.
    REQUIRE_ROOT_OR_SKIP();
    test::TempDir dir;
    const fs::path root = dir / "unplug-root";
    fs::create_directories(root);
    Rig rig({root.string()});
    ASSERT_TRUE(rig.rt);
    ASSERT_FALSE(rig.text_id.empty());
    const auto good_path = text_gguf_path(rig);

    const fs::path cut = root / "soak-cut.gguf";
    {
        std::ifstream in(good_path, std::ios::binary);
        std::ofstream out(cut, std::ios::binary);
        std::string buf(1u << 20, '\0');
        for (int i = 0; i < 32 && in; ++i) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            out.write(buf.data(), in.gcount());
        }
    }
    const fs::path vanished = root / "soak-vanished.gguf";
    test::write_file(vanished, test::fake_gguf());

    auto reg = [&](const std::string& id, const fs::path& p) {
        ModelManifest m;
        m.id = id;
        m.display_name = id;
        m.path = p.string();
        m.format = ModelFormat::Gguf;
        m.kind = ModelKind::Text;
        m.size_bytes = fs::file_size(p);
        auto r = rig.rt->registry.add(m);
        EXPECT_TRUE(r.ok()) << (r.ok() ? "" : r.error().message);
    };
    reg("soak-cut", cut);
    reg("soak-vanished", vanished);
    std::error_code ec;
    fs::remove(vanished, ec);  // "drive removed" after the registry saw the file

    const long long rounds = env_i("HCS_SOAK_UNPLUG_ROUNDS", 5);
    std::vector<Proc> samples;
    for (long long r = 0; r < rounds; ++r) {
        for (const char* id : {"soak-cut", "soak-vanished"}) {
            Result<void> l = Error(ErrorCode::Internal, "unset");
            ASSERT_NO_THROW(l = rig.cp().load_model(id));
            EXPECT_FALSE(l.ok()) << id << ": load of a truncated/missing model must fail";
            if (!l.ok()) {
                EXPECT_NE(l.error().code, ErrorCode::Internal) << id << " surfaced as an internal error";
                if (r == 0)
                    emit({{"test", "UnplugEngineLoad"},
                          {"kind", "case"},
                          {"id", id},
                          {"code", std::string(to_string(l.error().code))},
                          {"message", l.error().message.substr(0, 200)}});
            }
            EXPECT_FALSE(rig.cp().loaded_model().has_value()) << id;
            EXPECT_EQ(rig.rt->budget.reserved(), 0u) << id << ": budget not rolled back";
            auto st = rig.rt->registry.state(id);
            ASSERT_TRUE(st.ok());
            emit({{"test", "UnplugEngineLoad"},
                  {"kind", "state"},
                  {"id", id},
                  {"round", r},
                  {"state", std::string(to_string(st.value()))}});
        }
        samples.push_back(sample());
    }
    // recovery: the good model loads and generates after the failed loads
    ASSERT_TRUE(rig.cp().load_model(rig.text_id).ok()) << "engine unusable after failed loads";
    auto s = rig.cp().submit_chat(text_params(rig.text_id, 8));
    ASSERT_TRUE(s.ok());
    auto o = drain(rig.cp(), s.value(), 120s);
    EXPECT_EQ(o.state, JobState::Succeeded);
    EXPECT_FALSE(o.text.empty());
    check_growth("UnplugEngineLoad", samples, std::min<std::size_t>(1, samples.size() / 2));
}
