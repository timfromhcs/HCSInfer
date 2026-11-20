// Real-model shutdown checks (AGENTS.md 16.1 G, 16.2): the real llama.cpp adapter (CPU) behind the real HTTP
// server. Needs HCS_TEST_GGUF; without it every test is NOT RUN (skipped), never a fake pass.
#include <gtest/gtest.h>
#include <httplib.h>

#include <atomic>
#include <cstdlib>
#include <future>
#include <thread>

#include "hcs/api/http_api.hpp"
#include "hcs/api/runtime.hpp"
#include "temp_dir.hpp"

using namespace hcs;
using namespace hcs::api;
using nlohmann::json;
using namespace std::chrono_literals;

namespace {

const char* kToken = "real-shutdown-token-0123456789ab";

std::string env(const char* n) {
    const char* v = std::getenv(n);
    return v ? v : "";
}

#define REQUIRE_MODEL_OR_SKIP(var)                \
    const std::string var = env("HCS_TEST_GGUF"); \
    if (var.empty())                              \
    GTEST_SKIP() << "NOT RUN: HCS_TEST_GGUF is not set (real-model test)"

struct RealRig {
    test::TempDir dir;
    std::ostringstream sink;
    Logger logger{&sink, LogLevel::Debug};
    std::unique_ptr<Runtime> rt;
    std::unique_ptr<ApiHandler> handler;
    std::unique_ptr<HttpServer> server;
    std::string model_id;
    int port = 0;

    explicit RealRig(const std::string& gguf) {
        const std::filesystem::path p(gguf);
        RuntimeOptions ro;
        ro.state_dir = dir.path() / "state";
        std::filesystem::create_directories(ro.state_dir);
        ro.model_roots = {p.parent_path().string()};
        ro.control.backend = Backend::Cpu;  // deterministic: shutdown behaviour, not Vulkan, is under test
        ro.control.context_tokens = 1024;
        auto created = Runtime::create(std::move(ro), logger);
        EXPECT_TRUE(created.ok());
        if (!created)
            return;
        rt = std::move(created.value());
        (void)rt->registry.scan(p.parent_path(), rt->guard);
        for (const auto& m : rt->registry.list())
            if (std::filesystem::path(m.path).filename() == p.filename())
                model_id = m.id;
        ApiOptions ao;
        ao.token = kToken;
        ao.request_timeout = 300s;
        handler = std::make_unique<ApiHandler>(*rt->control, ao, logger);
        server = std::make_unique<HttpServer>(*handler, serve_transport_options(ao, "127.0.0.1", 0), logger);
        auto bound = server->start();
        EXPECT_TRUE(bound.ok());
        if (bound)
            port = bound.value();
    }
    httplib::Headers auth() const { return {{"Authorization", std::string("Bearer ") + kToken}}; }
    long long shutdown_within(std::chrono::seconds limit) {
        const auto t0 = std::chrono::steady_clock::now();
        auto fut = std::async(std::launch::async, [&] { graceful_shutdown(*server, *rt); });
        const bool done = fut.wait_for(limit) == std::future_status::ready;
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
                .count();
        if (!done) {
            rt->queue->shutdown();
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
        std::this_thread::sleep_for(10ms);
    }
    return pred();
}

}  // namespace

TEST(ShutdownReal, IdleWithALoadedModelShutsDownAndFreesTheEngine) {
    REQUIRE_MODEL_OR_SKIP(gguf);
    RealRig rig(gguf);
    ASSERT_TRUE(rig.rt && rig.port && !rig.model_id.empty());
    auto l = rig.rt->control->load_model(rig.model_id);
    ASSERT_TRUE(l.ok()) << l.error().message;
    const auto ms = rig.shutdown_within(30s);
    ASSERT_GE(ms, 0) << "idle shutdown with a real model hung";
    EXPECT_LT(ms, 15000);
    rig.rt.reset();  // destroys the engine with the model resident: must not crash or hang
    SUCCEED();
}

TEST(ShutdownReal, ShutdownDuringARealStreamingGenerationCancelsItPromptly) {
    REQUIRE_MODEL_OR_SKIP(gguf);
    RealRig rig(gguf);
    ASSERT_TRUE(rig.rt && rig.port && !rig.model_id.empty());
    ASSERT_TRUE(rig.rt->control->load_model(rig.model_id).ok());
    std::atomic<std::size_t> received{0};
    std::atomic<bool> client_done{false};
    std::thread client([&] {
        httplib::Client c("127.0.0.1", rig.port);
        c.set_read_timeout(300, 0);
        const json body = {
            {"model", rig.model_id},
            {"stream", true},
            {"max_tokens", 2048},
            {"temperature", 0.8},
            {"messages", json::array({json{{"role", "user"},
                                           {"content", "Write a very long story about a lighthouse."}}})}};
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
    ASSERT_TRUE(wait_for([&] { return received.load() > 0; }, 120s))
        << "no streamed bytes from the real model";
    const auto ms = rig.shutdown_within(60s);
    ASSERT_GE(ms, 0) << "shutdown during a real generation hung";
    EXPECT_LT(ms, 30000);
    client.join();
    EXPECT_TRUE(client_done.load());
    rig.rt.reset();
}

TEST(ShutdownReal, ShutdownWhileTheRealModelIsLoadingIsBoundedAndSafe) {
    REQUIRE_MODEL_OR_SKIP(gguf);
    RealRig rig(gguf);
    ASSERT_TRUE(rig.rt && rig.port && !rig.model_id.empty());
    httplib::Client c("127.0.0.1", rig.port);
    auto r = c.Post("/hcs/v1/models/" + rig.model_id + "/load", rig.auth(), "", "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 202) << r->body;
    // Do not wait: the load is (at best) in progress when shutdown starts. The weights read cannot be
    // interrupted, so shutdown is bounded by the load time; it must neither hang nor crash.
    const auto ms = rig.shutdown_within(120s);
    ASSERT_GE(ms, 0) << "shutdown during a real model load hung";
    rig.rt.reset();
    SUCCEED();
}
