// Contract tests for POST /hcs/v1/benchmarks. FakeTextEngine exercises the PLUMBING only (auth, schema,
// limits, job queue, result retrieval, cancellation, lifecycle). The numbers inside the fake's results are
// not benchmark evidence (AGENTS.md 16.2); the measurement itself is unit-tested in test_benchmark.cpp and
// proven on a real model through `HCSInfer benchmark`.
#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "api_fixture.hpp"
#include "hcs/api/http_api.hpp"

using namespace hcs;
using namespace hcs::api;
using nlohmann::json;

namespace {

const char* kToken = "test-token-0123456789abcdef";

struct Api {
    test::ApiFixture fx;
    ApiHandler handler;
    explicit Api(bool with_engine = true, ApiOptions o = {})
        : fx(with_engine), handler(*fx.cp, finish(std::move(o)), fx.logger) {}

    static ApiOptions finish(ApiOptions o) {
        o.token = kToken;
        o.port = 8765;
        return o;
    }
    HttpResponse call(const std::string& method, const std::string& path, const std::string& body = {},
                      bool auth = true, const std::string& content_type = "application/json") {
        HttpRequest r;
        r.method = method;
        r.path = path;
        r.body = body;
        r.headers["host"] = "127.0.0.1:8765";
        if (auth)
            r.headers["authorization"] = std::string("Bearer ") + kToken;
        if (!body.empty())
            r.headers["content-type"] = content_type;
        return handler.handle(r);
    }
    HttpResponse post(const json& body, bool auth = true) {
        return call("POST", "/hcs/v1/benchmarks", body.dump(), auth);
    }
    json job(const std::string& id) { return json::parse(call("GET", "/hcs/v1/jobs/" + id).body); }
    // Polls the job until it is terminal.
    json wait_job(const std::string& id, int ms = 10000) {
        for (int i = 0; i < ms / 10; ++i) {
            json j = job(id);
            const std::string st = j["state"];
            if (st == "succeeded" || st == "failed" || st == "cancelled")
                return j;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return job(id);
    }
};

json small_body(const std::string& suite = "quick") {
    return {{"suite", suite}, {"model", "tiny"}, {"warmup", 0}, {"iterations", 1}, {"max_tokens", 8}};
}

json err_of(const HttpResponse& r) {
    return json::parse(r.body)["error"];
}

}  // namespace

TEST(ApiBenchmarks, QuickSuiteRunsAsAJobAndTheResultIsRetrievableFromTheJob) {
    Api a;
    auto r = a.post(small_body());
    ASSERT_EQ(r.status, 202) << r.body;
    const json acc = json::parse(r.body);
    const std::string id = acc["job_id"];
    EXPECT_EQ(acc["status_url"], "/hcs/v1/jobs/" + id);
    EXPECT_EQ(acc["suite"], "quick");
    EXPECT_EQ(acc["model"], "tiny");

    const json j = a.wait_job(id);
    ASSERT_EQ(j["state"], "succeeded") << j.dump();
    EXPECT_EQ(j["name"], "benchmark.run");
    ASSERT_TRUE(j.contains("result"));
    const json& doc = j["result"];
    EXPECT_EQ(doc["schema_version"], 1);
    EXPECT_EQ(doc["suite"], "quick");
    EXPECT_EQ(doc["cold_or_warm"], "warm");
    EXPECT_EQ(doc["result_status"], "PASS");
    EXPECT_EQ(doc["model_file"], "tiny.gguf");
    EXPECT_EQ(doc["model_sha256"].is_string(), true);
    EXPECT_EQ(doc["measured_iterations"], 1);
    ASSERT_EQ(doc["samples"]["ttft_ms"]["samples"].size(), 1u);  // raw samples are kept
    EXPECT_TRUE(doc.contains("cold"));
    EXPECT_NE(doc["cold"]["note"].get<std::string>().find("server"), std::string::npos);  // not process-cold
    EXPECT_EQ(doc.dump().find("harbour"), std::string::npos);  // the prompt text never leaves the server

    // Cold load + one warm reload through the control plane; the benchmarked model stays loaded.
    EXPECT_EQ(a.fx.engine.loads.load(), 2);
    EXPECT_EQ(a.fx.cp->loaded_model().value_or(""), "tiny");
    EXPECT_GT(j["event_count"].get<int>(), 3);  // progress events were emitted
}

TEST(ApiBenchmarks, FullSuiteReturnsPrefillAndDecodeSweeps) {
    Api a;
    json body = small_body("full");
    body["sweep_sizes"] = {16, 32};
    auto r = a.post(body);
    ASSERT_EQ(r.status, 202) << r.body;
    const json j = a.wait_job(json::parse(r.body)["job_id"]);
    ASSERT_EQ(j["state"], "succeeded") << j.dump();
    const json& doc = j["result"];
    EXPECT_EQ(doc["suite"], "full");
    ASSERT_EQ(doc["prefill_sweep"].size(), 2u);
    ASSERT_EQ(doc["decode_sweep"].size(), 2u);
    EXPECT_EQ(doc["prefill_sweep"][0]["target_prompt_tokens"], 16);
    EXPECT_EQ(doc["decode_sweep"][1]["target_prompt_tokens"], 32);
    EXPECT_EQ(doc["decode_sweep"][0]["result_status"], "PASS");
}

TEST(ApiBenchmarks, DefaultsToQuickAndModelIsRequired) {
    Api a;
    auto ok = a.post({{"model", "tiny"}, {"warmup", 0}, {"iterations", 1}});
    EXPECT_EQ(ok.status, 202) << ok.body;
    EXPECT_EQ(json::parse(ok.body)["suite"], "quick");
    a.wait_job(json::parse(ok.body)["job_id"]);
    auto missing = a.post({{"suite", "quick"}});
    EXPECT_EQ(missing.status, 400);
    EXPECT_EQ(err_of(missing)["code"], "invalid_request");
}

TEST(ApiBenchmarks, MalformedAndOutOfSchemaBodiesAre400) {
    Api a;
    EXPECT_EQ(a.call("POST", "/hcs/v1/benchmarks", "{not json").status, 400);
    EXPECT_EQ(a.call("POST", "/hcs/v1/benchmarks", "[1,2]").status, 400);
    EXPECT_EQ(a.call("POST", "/hcs/v1/benchmarks", std::string(40, '[') + std::string(40, ']')).status, 400);
    json b = small_body();
    b["unknown_field"] = 1;  // strict schema: no silent extras
    EXPECT_EQ(a.post(b).status, 400);
    for (const json& bad :
         {json{{"suite", "slow"}}, json{{"iterations", 0}}, json{{"iterations", 51}}, json{{"warmup", 21}},
          json{{"max_tokens", 0}}, json{{"max_tokens", 4097}}, json{{"iterations", 1.5}}, json{{"seed", -1}},
          json{{"model", ""}}, json{{"model", 7}}, json{{"sweep_sizes", json::array()}},
          json{{"sweep_sizes", {15}}}, json{{"sweep_sizes", {1048577}}},
          json{{"sweep_sizes", {16, 16, 16, 16, 16, 16, 16, 16, 16}}}}) {
        json body = small_body("full");
        for (auto it = bad.begin(); it != bad.end(); ++it) body[it.key()] = it.value();
        EXPECT_EQ(a.post(body).status, 400) << body.dump();
    }
    // sweep_sizes only makes sense for the full suite
    json q = small_body("quick");
    q["sweep_sizes"] = {64};
    EXPECT_EQ(a.post(q).status, 400);
    EXPECT_EQ(a.fx.engine.loads.load(), 0);  // nothing was started by any rejected request
}

TEST(ApiBenchmarks, WrongContentTypeMethodAndRoute) {
    Api a;
    EXPECT_EQ(a.call("POST", "/hcs/v1/benchmarks", small_body().dump(), true, "text/plain").status, 415);
    auto get = a.call("GET", "/hcs/v1/benchmarks");
    EXPECT_EQ(get.status, 405);
    bool allow = false;
    for (auto& [k, v] : get.headers) allow = allow || (k == "Allow" && v == "POST");
    EXPECT_TRUE(allow);
    EXPECT_EQ(a.call("POST", "/hcs/v1/benchmarks/extra", "{}").status, 404);
}

TEST(ApiBenchmarks, OversizedBodyIs413BeforeParsing) {
    Api a;
    json b = small_body();
    b["model"] = std::string(5000, 'a');
    auto r = a.post(b);
    EXPECT_EQ(r.status, 413);
    EXPECT_EQ(err_of(r)["code"], "payload_too_large");
}

TEST(ApiBenchmarks, UnknownOrInvalidModelIsRejectedBeforeAnyJobStarts) {
    Api a;
    json b = small_body();
    b["model"] = "no-such-model";
    auto r = a.post(b);
    EXPECT_EQ(r.status, 404);
    EXPECT_EQ(err_of(r)["code"], "not_found");
    b["model"] = "../../etc/passwd";
    EXPECT_EQ(a.post(b).status, 400);
    b["model"] = "C:\\models\\tiny.gguf";  // a path is not a model id
    EXPECT_EQ(a.post(b).status, 400);
    EXPECT_EQ(a.fx.engine.loads.load(), 0);
    EXPECT_EQ(a.fx.queue->pending(), 0u);
}

TEST(ApiBenchmarks, RequiresBearerToken) {
    Api a;
    auto r = a.post(small_body(), false);
    EXPECT_EQ(r.status, 401);
    EXPECT_EQ(err_of(r)["code"], "unauthorized");
    EXPECT_EQ(a.fx.engine.loads.load(), 0);
}

TEST(ApiBenchmarks, WithoutATextEngineTheRouteSaysUnsupportedInsteadOfPretending) {
    Api a(/*with_engine=*/false);
    auto r = a.post(small_body());
    EXPECT_EQ(r.status, 501);
    EXPECT_EQ(err_of(r)["code"], "unsupported");
}

TEST(ApiBenchmarks, LoadFailureEndsTheJobFailedWithAnError) {
    Api a;
    a.fx.engine.fail_load = true;
    auto r = a.post(small_body());
    ASSERT_EQ(r.status, 202) << r.body;
    const json j = a.wait_job(json::parse(r.body)["job_id"]);
    EXPECT_EQ(j["state"], "failed");
    EXPECT_EQ(j["error"]["code"], "model_error");
    EXPECT_FALSE(j.contains("result"));
}

TEST(ApiBenchmarks, CancelEndpointStopsTheRunningBenchmark) {
    Api a;
    a.fx.engine.run_until_cancel = true;  // the "engine" generates until cancelled
    auto r = a.post(small_body());
    ASSERT_EQ(r.status, 202) << r.body;
    const std::string id = json::parse(r.body)["job_id"];
    for (int i = 0; i < 1000 && !a.fx.engine.generating.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_TRUE(a.fx.engine.generating.load()) << "the benchmark never reached the engine";

    auto c = a.call("POST", "/hcs/v1/jobs/" + id + "/cancel");
    EXPECT_EQ(c.status, 200) << c.body;
    const json j = a.wait_job(id);
    EXPECT_EQ(j["state"], "cancelled");
    EXPECT_FALSE(j.contains("result"));  // a cancelled run publishes no numbers
    for (int i = 0; i < 400 && a.fx.engine.generating.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_FALSE(a.fx.engine.generating.load());  // the engine really stopped
    a.fx.engine.run_until_cancel = false;
    // The control plane is usable again: the model is in a consistent state and chat works.
    EXPECT_EQ(
        a.call("POST", "/v1/chat/completions",
               json{{"model", "tiny"}, {"messages", json::array({json{{"role", "user"}, {"content", "hi"}}})}}
                   .dump())
            .status,
        200);
}

TEST(ApiBenchmarks, CancellingAQueuedBenchmarkNeverTouchesTheEngine) {
    Api a;
    // Occupy the single job worker with a long-running chat so the benchmark stays queued.
    a.fx.engine.run_until_cancel = true;
    std::thread chat([&] {
        a.call("POST", "/v1/chat/completions",
               json{{"model", "tiny"}, {"messages", json::array({json{{"role", "user"}, {"content", "hi"}}})}}
                   .dump());
    });
    for (int i = 0; i < 1000 && !a.fx.engine.generating.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_TRUE(a.fx.engine.generating.load());
    const int loads_before = a.fx.engine.loads.load();
    auto r = a.post(small_body());
    ASSERT_EQ(r.status, 202) << r.body;
    const std::string id = json::parse(r.body)["job_id"];
    EXPECT_EQ(a.job(id)["state"], "queued");
    EXPECT_EQ(a.call("POST", "/hcs/v1/jobs/" + id + "/cancel").status, 200);
    EXPECT_EQ(a.job(id)["state"], "cancelled");
    EXPECT_EQ(a.fx.engine.loads.load(), loads_before);
    // free the worker: cancel the chat job (job-1)
    EXPECT_EQ(a.call("POST", "/hcs/v1/jobs/job-1/cancel").status, 200);
    chat.join();
    a.fx.engine.run_until_cancel = false;
}
