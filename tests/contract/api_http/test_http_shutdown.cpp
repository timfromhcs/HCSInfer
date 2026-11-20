// Shutdown while idle / loading / generating on the control plane and the real HTTP server (AGENTS.md 16.1 G,
// 19.5). graceful_shutdown() must return in bounded time, never crash or hang, cancel what is cancellable and
// leave the process able to destroy the Runtime. FakeTextEngine = lifecycle plumbing only; the real-model
// variants live in tests/e2e and are NOT RUN unless HCS_TEST_GGUF is configured (see the evidence README).
#include <gtest/gtest.h>
#include <httplib.h>

#include <atomic>
#include <future>
#include <thread>

#include "fake_text_engine.hpp"
#include "hcs/api/http_api.hpp"
#include "hcs/api/runtime.hpp"
#include "temp_dir.hpp"

using namespace hcs;
using namespace hcs::api;
using nlohmann::json;
using namespace std::chrono_literals;

namespace {

const char* kToken = "shutdown-token-0123456789abcdef";

struct ShutdownRig {
    test::TempDir dir;
    std::ostringstream sink;
    Logger logger{&sink, LogLevel::Debug};
    std::unique_ptr<Runtime> rt;
    test::FakeTextEngine* fake = nullptr;  // owned by rt
    std::string model_id;
    std::unique_ptr<ApiHandler> handler;
    std::unique_ptr<HttpServer> server;
    int port = 0;

    ShutdownRig(bool run_until_cancel, int load_delay_ms = 0) {
        const auto root = dir.path() / "models";
        test::write_file(root / "tiny.gguf", test::fake_gguf());
        RuntimeOptions ro;
        ro.state_dir = dir.path() / "state";
        std::filesystem::create_directories(ro.state_dir);
        ro.model_roots = {root.string()};
        auto eng = std::make_unique<test::FakeTextEngine>();
        eng->run_until_cancel = run_until_cancel;
        eng->delay_ms = run_until_cancel ? 2 : 0;
        eng->load_delay_ms = load_delay_ms;
        fake = eng.get();
        ro.engine = std::move(eng);
        auto created = Runtime::create(std::move(ro), logger);
        EXPECT_TRUE(created.ok());
        if (!created)
            return;
        rt = std::move(created.value());
        (void)rt->registry.scan(root, rt->guard);
        const auto models = rt->registry.list();
        if (!models.empty())
            model_id = models.front().id;
        ApiOptions ao;
        ao.token = kToken;
        ao.request_timeout = 300s;  // shutdown must not depend on this
        handler = std::make_unique<ApiHandler>(*rt->control, ao, logger);
        server = std::make_unique<HttpServer>(*handler, serve_transport_options(ao, "127.0.0.1", 0), logger);
        auto bound = server->start();
        EXPECT_TRUE(bound.ok());
        if (bound)
            port = bound.value();
    }
    httplib::Headers auth() const { return {{"Authorization", std::string("Bearer ") + kToken}}; }

    // Runs graceful_shutdown on a watchdog; returns elapsed ms, or -1 when it did not finish in `limit`.
    long long shutdown_within(std::chrono::seconds limit) {
        const auto t0 = std::chrono::steady_clock::now();
        auto fut = std::async(std::launch::async, [&] { graceful_shutdown(*server, *rt); });
        const bool done = fut.wait_for(limit) == std::future_status::ready;
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
                .count();
        if (!done) {
            rt->queue->shutdown();  // unblock so the process can exit; the caller fails the test
            fut.wait();
            return -1;
        }
        return ms;
    }
};

bool wait_for(const std::function<bool()>& pred, std::chrono::milliseconds limit) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
        if (pred())
            return true;
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}

}  // namespace

// ---- idle
// ---------------------------------------------------------------------------------------------------
TEST(ShutdownIdle, NoModelNoJobsStopsPromptlyAndIsIdempotent) {
    ShutdownRig rig(false);
    ASSERT_TRUE(rig.rt && rig.port);
    const auto ms = rig.shutdown_within(10s);
    ASSERT_GE(ms, 0) << "idle shutdown hung";
    EXPECT_LT(ms, 5000);
    // Calling it again (SIGINT twice) and then destroying the runtime stays safe.
    EXPECT_GE(rig.shutdown_within(10s), 0);
    httplib::Client c("127.0.0.1", rig.port);
    c.set_connection_timeout(0, 300000);
    EXPECT_FALSE(c.Get("/hcs/v1/health")) << "the port still answers after shutdown";
    rig.rt.reset();
}

TEST(ShutdownIdle, LoadedModelAndAKeepAliveClientDoNotBlockShutdown) {
    ShutdownRig rig(false);
    ASSERT_TRUE(rig.rt && rig.port);
    ASSERT_TRUE(rig.rt->control->load_model(rig.model_id).ok());
    httplib::Client c("127.0.0.1", rig.port);
    c.set_keep_alive(true);
    auto h = c.Get("/hcs/v1/health");
    ASSERT_TRUE(h);
    EXPECT_EQ(h->status, 200);
    const auto ms = rig.shutdown_within(10s);  // the keep-alive connection is still open
    ASSERT_GE(ms, 0) << "shutdown waited for an idle keep-alive connection";
    EXPECT_LT(ms, 5000);
    rig.rt.reset();  // destroys the engine with a model still loaded
    SUCCEED();
}

// ---- loading
// ------------------------------------------------------------------------------------------------
TEST(ShutdownLoading, ShutdownDuringAnUninterruptibleLoadWaitsForItAndNeverCrashes) {
    ShutdownRig rig(false, /*load_delay_ms=*/1200);
    ASSERT_TRUE(rig.rt && rig.port);
    httplib::Client c("127.0.0.1", rig.port);
    auto r = c.Post("/hcs/v1/models/" + rig.model_id + "/load", rig.auth(), "", "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 202) << r->body;
    ASSERT_TRUE(wait_for([&] { return rig.fake->loading.load(); }, 5s))
        << "the load never reached the engine";
    // The engine load is a blocking call (an mmap/weights read) that cannot be interrupted: shutdown is
    // bounded by it, not hung, and must not tear the engine down underneath it.
    const auto ms = rig.shutdown_within(20s);
    ASSERT_GE(ms, 0) << "shutdown during a model load hung";
    EXPECT_LT(ms, 15000);
    EXPECT_FALSE(rig.fake->loading.load()) << "shutdown returned while the engine load was still running";
    rig.rt.reset();
    SUCCEED();
}

TEST(ShutdownLoading, QueuedLoadThatNeverStartsIsCancelledNotRunAfterShutdown) {
    ShutdownRig rig(false, /*load_delay_ms=*/800);
    ASSERT_TRUE(rig.rt && rig.port);
    auto a = rig.rt->control->submit_load(rig.model_id);
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(wait_for([&] { return rig.fake->loading.load(); }, 5s));
    // Second job queued behind the running load (single worker).
    ChatParams p;
    p.model = rig.model_id;
    p.messages.push_back({"user", "hi", {}});
    auto queued = rig.rt->control->submit_chat(p);
    ASSERT_TRUE(queued.ok());
    ASSERT_GE(rig.shutdown_within(20s), 0);
    auto st = rig.rt->control->job_status(queued.value().id);
    ASSERT_TRUE(st.ok());
    EXPECT_NE(st.value().state, JobState::Running);
    EXPECT_NE(st.value().state, JobState::Queued) << "a queued job was left dangling after shutdown";
    rig.rt.reset();
}

TEST(ShutdownLoading, UnloadRequestedWhileALoadRunsSerialisesAfterItAndEndsUnloaded) {
    ShutdownRig rig(false, /*load_delay_ms=*/600);
    ASSERT_TRUE(rig.rt);
    auto job = rig.rt->control->submit_load(rig.model_id);
    ASSERT_TRUE(job.ok());
    ASSERT_TRUE(wait_for([&] { return rig.fake->loading.load(); }, 5s));
    auto u = rig.rt->control->unload_model();  // blocks on the engine mutex until the load finished
    // Either the load had already published (unload succeeds) or the load was cancelled before it did
    // (nothing to unload => InvalidState). Both are structured outcomes; neither may crash or wedge the
    // model.
    EXPECT_TRUE(u.ok() || u.error().code == ErrorCode::InvalidState) << (u ? "" : u.error().message);
    EXPECT_FALSE(rig.rt->control->loaded_model().has_value());
    ASSERT_TRUE(rig.rt->control->load_model(rig.model_id).ok())
        << "the model is wedged after load/unload race";
}

// ---- generating
// ---------------------------------------------------------------------------------------------
TEST(ShutdownGenerating, StreamingClientIsEndedAndShutdownIsPrompt) {
    ShutdownRig rig(/*run_until_cancel=*/true);
    ASSERT_TRUE(rig.rt && rig.port);
    std::atomic<std::size_t> received{0};
    std::atomic<bool> client_done{false};
    std::thread client([&] {
        httplib::Client c("127.0.0.1", rig.port);
        c.set_read_timeout(120, 0);
        const json body = {{"model", rig.model_id},
                           {"stream", true},
                           {"messages", json::array({json{{"role", "user"}, {"content", "hi"}}})}};
        httplib::Request req;
        req.method = "POST";
        req.path = "/v1/chat/completions";
        req.headers = rig.auth();
        req.set_header("Content-Type", "application/json");
        req.body = body.dump();
        req.content_receiver = [&](const char*, size_t n, uint64_t, uint64_t) {
            received += n;
            return true;
        };
        (void)c.send(req);
        client_done = true;
    });
    ASSERT_TRUE(wait_for([&] { return rig.fake->generating.load() && received.load() > 0; }, 10s))
        << "stream never started";
    const auto ms = rig.shutdown_within(15s);
    ASSERT_GE(ms, 0) << "shutdown during a streaming generation hung";
    EXPECT_LT(ms, 10000);
    client.join();
    EXPECT_TRUE(client_done.load());
    EXPECT_FALSE(rig.fake->generating.load()) << "generation still running after shutdown";
    rig.rt.reset();
}

TEST(ShutdownGenerating, ControlPlaneUnloadCancelsTheRunningJobThenTheNextChatWorks) {
    ShutdownRig rig(/*run_until_cancel=*/true);
    ASSERT_TRUE(rig.rt);
    ChatParams p;
    p.model = rig.model_id;
    p.messages.push_back({"user", "hi", {}});
    auto sub = rig.rt->control->submit_chat(p);
    ASSERT_TRUE(sub.ok());
    ASSERT_TRUE(wait_for([&] { return rig.fake->generating.load(); }, 10s));
    auto fut = std::async(std::launch::async, [&] { return rig.rt->control->unload_model(); });
    ASSERT_EQ(fut.wait_for(15s), std::future_status::ready) << "unload during generation hung";
    EXPECT_TRUE(fut.get().ok());
    ASSERT_TRUE(rig.rt->control->wait_job(sub.value().id, 10s));
    auto st = rig.rt->control->job_status(sub.value().id);
    ASSERT_TRUE(st.ok());
    EXPECT_EQ(st.value().state, JobState::Cancelled);
    EXPECT_FALSE(rig.fake->generating.load());
    EXPECT_FALSE(rig.rt->control->loaded_model().has_value());
    rig.fake->run_until_cancel = false;
    ASSERT_TRUE(rig.rt->control->load_model(rig.model_id).ok());
}
