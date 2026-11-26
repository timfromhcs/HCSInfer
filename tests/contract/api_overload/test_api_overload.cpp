// Backpressure over a real HTTP server (plan F1). The engine is FakeTextEngine and the memory probe is an
// injected fake: this tests the admission / Retry-After / kill-switch plumbing, not inference or hardware.
#include <gtest/gtest.h>
#include <httplib.h>

#include <atomic>
#include <future>
#include <thread>

#include "api_fixture.hpp"
#include "fake_probe.hpp"
#include "hcs/api/http_api.hpp"

using namespace hcs;
using namespace hcs::api;
using hcs::test::kGiB;
using hcs::test::kMiB;
using nlohmann::json;

namespace {

const char* kToken = "overload-token-0123456789abcdef";

api::ControlPlaneOptions with_admission(std::shared_ptr<scheduler::AdmissionController> a) {
    api::ControlPlaneOptions o;
    o.admission = std::move(a);
    return o;
}

struct Server {
    test::FakeProbe probe;
    std::shared_ptr<scheduler::AdmissionController> adm;
    test::ApiFixture fx;
    ApiHandler handler;
    HttpServer server;
    int port = 0;

    static scheduler::AdmissionConfig config(std::size_t max_in_flight) {
        scheduler::AdmissionConfig c;
        c.max_in_flight = max_in_flight;
        c.retry_after = std::chrono::seconds(3);
        c.shed_deadline = std::chrono::milliseconds(5000);
        return c;
    }
    static ApiOptions api_opts() {
        ApiOptions o;
        o.token = kToken;
        o.max_inflight = 16;  // the HTTP in-flight cap must not be what trips here
        return o;
    }
    static TransportOptions topts() {
        TransportOptions t;
        t.host = "127.0.0.1";
        t.port = 0;
        t.threads = 12;
        return t;
    }
    explicit Server(std::size_t max_in_flight = 2)
        : adm(std::make_shared<scheduler::AdmissionController>(config(max_in_flight), probe.probe())),
          fx(true, with_admission(adm)),
          handler(*fx.cp, api_opts(), fx.logger),
          server(handler, topts(), fx.logger) {
        auto p = server.start();
        EXPECT_TRUE(p.ok());
        if (p)
            port = p.value();
    }
    ~Server() {
        server.stop();
        fx.queue->shutdown();
    }
    httplib::Headers auth() const { return {{"Authorization", std::string("Bearer ") + kToken}}; }
};

std::string chat_body() {
    return json{{"model", "tiny"},
                {"stream", false},
                {"messages", json::array({json{{"role", "user"}, {"content", "hi"}}})}}
        .dump();
}

bool wait_for(const std::function<bool()>& f, int ms = 5000) {
    for (int i = 0; i < ms / 5; ++i) {
        if (f())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return f();
}

}  // namespace

TEST(ApiOverload, WithoutPressureChatStillWorks) {
    Server s;
    httplib::Client c("127.0.0.1", s.port);
    auto r = c.Post("/v1/chat/completions", s.auth(), chat_body(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200) << r->body;
    // The slot is released when the job is done.
    EXPECT_TRUE(wait_for([&] { return s.adm->in_flight() == 0; }));
}

TEST(ApiOverload, MemoryPressureAnswers503WithRetryAfter) {
    Server s;
    s.probe.set_free(2000 * kMiB);  // below the 2.5 GiB soft floor
    httplib::Client c("127.0.0.1", s.port);
    auto r = c.Post("/v1/chat/completions", s.auth(), chat_body(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 503);
    EXPECT_EQ(r->get_header_value("Retry-After"), "3");
    const auto j = json::parse(r->body);
    EXPECT_EQ(j["error"]["code"], "overloaded");
    EXPECT_EQ(j["error"]["type"], "server_error");
    EXPECT_FALSE(j["error"]["request_id"].get<std::string>().empty());
    // Back to normal: served again (with hysteresis the free RAM must clear the band).
    s.probe.set_free(8 * kGiB);
    auto ok = c.Post("/v1/chat/completions", s.auth(), chat_body(), "application/json");
    ASSERT_TRUE(ok);
    EXPECT_EQ(ok->status, 200) << ok->body;
}

TEST(ApiOverload, ModelLoadThatBreaksTheRamFloorIsRefusedAsOutOfMemory) {
    Server s;
    // tiny.gguf is a few KiB; the KV estimate dominates. Leave just under the floor + tiny need.
    s.probe.set_free(2560 * kMiB + 1);  // only 1 byte above the floor: any positive need breaks it
    httplib::Client c("127.0.0.1", s.port);
    auto r = c.Post("/hcs/v1/models/tiny/load", s.auth(), "", "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 202) << r->body;
    const std::string job = json::parse(r->body)["job_id"];
    ASSERT_TRUE(wait_for([&] {
        auto st = c.Get("/hcs/v1/jobs/" + job, s.auth());
        return st && json::parse(st->body)["state"] == "failed";
    }));
    auto st = c.Get("/hcs/v1/jobs/" + job, s.auth());
    const auto j = json::parse(st->body);
    EXPECT_EQ(j["error"]["code"], "out_of_memory");
    EXPECT_EQ(s.fx.engine.loads.load(), 0);  // the engine was never touched
    EXPECT_FALSE(s.fx.cp->loaded_model().has_value());
}

TEST(ApiOverload, ConcurrencyBoundAnswers503WhileRunningJobsContinue) {
    Server s(2);
    s.fx.engine.run_until_cancel = true;  // each chat runs until it is cancelled
    s.fx.engine.delay_ms = 5;
    std::vector<std::future<int>> running;
    for (int i = 0; i < 2; ++i)
        running.push_back(std::async(std::launch::async, [&] {
            httplib::Client c("127.0.0.1", s.port);
            c.set_read_timeout(20, 0);
            auto r = c.Post("/v1/chat/completions", s.auth(), chat_body(), "application/json");
            return r ? r->status : -1;
        }));
    ASSERT_TRUE(wait_for([&] { return s.adm->in_flight() == 2; }));
    httplib::Client c("127.0.0.1", s.port);
    auto over = c.Post("/v1/chat/completions", s.auth(), chat_body(), "application/json");
    ASSERT_TRUE(over);
    EXPECT_EQ(over->status, 503);
    EXPECT_EQ(over->get_header_value("Retry-After"), "3");
    EXPECT_EQ(json::parse(over->body)["error"]["code"], "overloaded");
    // The controller stays responsive while saturated.
    auto snap = c.Get("/hcs/v1/runtime/overload", s.auth());
    ASSERT_TRUE(snap);
    EXPECT_EQ(snap->status, 200);
    EXPECT_EQ(json::parse(snap->body)["in_flight"], 2);
    EXPECT_GE(json::parse(snap->body)["counters"]["rejected_overloaded"].get<int>(), 1);
    // Kill switch frees everything.
    auto shed = c.Post("/hcs/v1/runtime/shed", s.auth(), "", "application/json");
    ASSERT_TRUE(shed);
    ASSERT_EQ(shed->status, 200) << shed->body;
    EXPECT_GE(json::parse(shed->body)["jobs_cancelled"].get<int>(), 1);
    for (auto& f : running) EXPECT_NE(f.get(), -1);
    EXPECT_TRUE(wait_for([&] { return s.adm->in_flight() == 0; }));
}

TEST(ApiOverload, KillSwitchCancelsRunningJobsAndUnloadsTheModelWithinTheDeadline) {
    Server s(4);
    httplib::Client c("127.0.0.1", s.port);
    // Load the model (admitted: plenty of RAM), then start an endless generation.
    auto ld = c.Post("/hcs/v1/models/tiny/load", s.auth(), "", "application/json");
    ASSERT_TRUE(ld);
    ASSERT_EQ(ld->status, 202);
    ASSERT_TRUE(wait_for([&] { return s.fx.cp->loaded_model().has_value(); }));
    s.fx.engine.run_until_cancel = true;
    s.fx.engine.delay_ms = 5;
    auto gen = std::async(std::launch::async, [&] {
        httplib::Client gc("127.0.0.1", s.port);
        gc.set_read_timeout(20, 0);
        auto r = gc.Post("/v1/chat/completions", s.auth(), chat_body(), "application/json");
        return r ? r->status : -1;
    });
    ASSERT_TRUE(wait_for([&] { return s.fx.engine.generating.load(); }));

    const auto t0 = std::chrono::steady_clock::now();
    auto shed = c.Post("/hcs/v1/runtime/shed", s.auth(), "", "application/json");
    const auto took = std::chrono::steady_clock::now() - t0;
    ASSERT_TRUE(shed);
    ASSERT_EQ(shed->status, 200) << shed->body;
    const auto j = json::parse(shed->body);
    EXPECT_GE(j["jobs_cancelled"].get<int>(), 1);
    EXPECT_TRUE(j["model_unloaded"].get<bool>());
    EXPECT_FALSE(j["unload_pending"].get<bool>());
    EXPECT_TRUE(j["jobs_idle"].get<bool>());
    EXPECT_LT(took, std::chrono::seconds(5));  // bounded by the configured deadline
    EXPECT_FALSE(s.fx.cp->loaded_model().has_value());
    EXPECT_GE(s.fx.engine.unloads.load(), 1);
    (void)gen.get();  // the cancelled request returned
    // Auth is still required and the route is POST-only.
    httplib::Client c2("127.0.0.1", s.port);
    auto unauth = c2.Post("/hcs/v1/runtime/shed", "", "application/json");
    ASSERT_TRUE(unauth);
    EXPECT_EQ(unauth->status, 401);
    auto get = c.Get("/hcs/v1/runtime/shed", s.auth());
    ASSERT_TRUE(get);
    EXPECT_EQ(get->status, 405);
    // The overload snapshot records the shed.
    auto snap = c.Get("/hcs/v1/runtime/overload", s.auth());
    ASSERT_TRUE(snap);
    EXPECT_EQ(json::parse(snap->body)["counters"]["shed_count"], 1);
}

TEST(ApiOverload, WatchdogTripsTheKillSwitchWhenFreeRamCollapses) {
    Server s(4);
    s.adm->start_watchdog();
    httplib::Client c("127.0.0.1", s.port);
    ASSERT_TRUE(c.Post("/hcs/v1/models/tiny/load", s.auth(), "", "application/json"));
    ASSERT_TRUE(wait_for([&] { return s.fx.cp->loaded_model().has_value(); }));
    s.probe.set_free(200 * kMiB);  // far below the 1 GiB hard floor
    EXPECT_TRUE(wait_for([&] { return !s.fx.cp->loaded_model().has_value(); }, 8000));
    auto snap = c.Get("/hcs/v1/runtime/overload", s.auth());
    ASSERT_TRUE(snap);
    const auto j = json::parse(snap->body);
    EXPECT_EQ(j["level"], "critical");
    EXPECT_EQ(j["counters"]["shed_count"], 1);
    EXPECT_NE(j["last_shed"]["reason"].get<std::string>().find("watchdog"), std::string::npos);
    // While critical the API refuses new inference with 503 + Retry-After but keeps answering health.
    auto refused = c.Post("/v1/chat/completions", s.auth(), chat_body(), "application/json");
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused->status, 503);
    EXPECT_FALSE(refused->get_header_value("Retry-After").empty());
    auto health = c.Get("/hcs/v1/health");
    ASSERT_TRUE(health);
    EXPECT_EQ(health->status, 200);
}

TEST(ApiOverload, RuntimeEndpointExposesTheOverloadState) {
    Server s;
    httplib::Client c("127.0.0.1", s.port);
    auto r = c.Get("/hcs/v1/runtime", s.auth());
    ASSERT_TRUE(r);
    const auto j = json::parse(r->body);
    ASSERT_TRUE(j.contains("overload"));
    EXPECT_EQ(j["overload"]["enabled"], true);
    EXPECT_EQ(j["overload"]["level"], "normal");
    EXPECT_EQ(j["overload"]["limits"]["min_free_ram_bytes"], 2560 * kMiB);
}

TEST(ApiOverload, WithoutAControllerTheShedRouteStillWorksAndSnapshotSaysDisabled) {
    test::ApiFixture fx;
    ApiOptions ao = Server::api_opts();
    ApiHandler handler(*fx.cp, ao, fx.logger);
    TransportOptions t = Server::topts();
    HttpServer server(handler, t, fx.logger);
    auto p = server.start();
    ASSERT_TRUE(p.ok());
    httplib::Client c("127.0.0.1", p.value());
    httplib::Headers h{{"Authorization", std::string("Bearer ") + kToken}};
    auto snap = c.Get("/hcs/v1/runtime/overload", h);
    ASSERT_TRUE(snap);
    EXPECT_EQ(json::parse(snap->body)["enabled"], false);
    auto shed = c.Post("/hcs/v1/runtime/shed", h, "", "application/json");
    ASSERT_TRUE(shed);
    EXPECT_EQ(shed->status, 200);
    server.stop();
    fx.queue->shutdown();
}

TEST(ApiOverloadShed, ReportsUnloadPendingWhenAnEngineLoadCannotBeInterrupted) {
    test::FakeProbe probe;
    scheduler::AdmissionConfig cfg;
    cfg.shed_deadline = std::chrono::milliseconds(300);
    auto adm = std::make_shared<scheduler::AdmissionController>(cfg, probe.probe());
    test::ApiFixture fx(true, with_admission(adm));
    fx.engine.load_delay_ms = 1500;  // a slow, uninterruptible weight read
    auto job = fx.cp->submit_load("tiny");
    ASSERT_TRUE(job.ok());
    ASSERT_TRUE(wait_for([&] { return fx.engine.loading.load(); }));
    // The engine load is a blocking call that cannot be interrupted: the shed must still return inside its
    // deadline and say so (jobs_idle == false) instead of hanging or pretending everything stopped.
    const auto t0 = std::chrono::steady_clock::now();
    const auto rep = fx.cp->shed("test");
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::milliseconds(1200));  // bounded
    EXPECT_GE(rep.jobs_cancelled, 1u);
    EXPECT_FALSE(rep.jobs_idle);  // the load ignores cancellation: reported honestly, not hidden
    // Once the engine returns, the cancelled load must NOT publish the model (the kill switch wins).
    EXPECT_TRUE(fx.cp->wait_job(job.value(), std::chrono::seconds(10)));
    EXPECT_FALSE(fx.cp->loaded_model().has_value());
    EXPECT_GE(fx.engine.unloads.load(), 1);  // the half-finished load was torn down
    EXPECT_EQ(fx.cp->job_status(job.value()).value().state, JobState::Cancelled);
}
