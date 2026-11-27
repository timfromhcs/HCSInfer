// `HCSInfer models load|unload|status` against a REAL HttpServer on a loopback port (real sockets, real
// bearer auth, real job polling). The engine behind the server is FakeTextEngine: this proves the CLI client,
// exit-code mapping and server wiring, not inference.
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>

#include "api_fixture.hpp"
#include "hcs/api/http_api.hpp"
#include "hcs/cli/cli.hpp"
#include "hcs/result.hpp"

using namespace hcs;
using nlohmann::json;

namespace {

const char* kToken = "cli-remote-token-0123456789abcdef";

struct Run {
    int code;
    std::string out, err;
};
Run run(std::vector<std::string> args) {
    std::ostringstream o, e;
    int c = cli::run(args, o, e);
    return {c, o.str(), e.str()};
}
bool contains(const std::string& s, const std::string& what) {
    return s.find(what) != std::string::npos;
}

struct Server {
    test::ApiFixture fx;
    api::ApiHandler handler;
    api::HttpServer server;
    int port = 0;

    static api::ApiOptions opts() {
        api::ApiOptions o;
        o.token = kToken;
        return o;
    }
    static api::TransportOptions topts() {
        api::TransportOptions t;
        t.host = "127.0.0.1";
        t.port = 0;
        t.threads = 4;
        return t;
    }
    Server() : handler(*fx.cp, opts(), fx.logger), server(handler, topts(), fx.logger) {
        auto p = server.start();
        EXPECT_TRUE(p.ok());
        if (p)
            port = p.value();
    }
    ~Server() { server.stop(); }
    std::vector<std::string> conn(std::vector<std::string> head, bool with_token = true) const {
        head.insert(head.end(), {"--port", std::to_string(port)});
        if (with_token)
            head.insert(head.end(), {"--token", kToken});
        return head;
    }
};

}  // namespace

TEST(CliRemote, HelpWorksAndNamesTheServer) {
    for (const char* sub : {"load", "unload", "status"}) {
        auto r = run({"models", sub, "--help"});
        EXPECT_EQ(r.code, 0) << sub;
        EXPECT_TRUE(contains(r.out, "RUNNING `HCSInfer serve`")) << sub;
    }
    EXPECT_TRUE(contains(run({"models", "--help"}).out, "status"));
    EXPECT_FALSE(contains(run({"--help"}).out, "Not in the one-shot CLI"));
}

TEST(CliRemote, NoServerRunningIsAClearIoErrorNotAStub) {
    int dead_port = 0;
    {
        Server s;
        dead_port = s.port;
    }  // stopped: nothing listens there any more
    auto r = run({"models", "status", "--port", std::to_string(dead_port)});
    EXPECT_EQ(r.code, exit_code(ErrorCode::IoError)) << r.err;
    EXPECT_TRUE(contains(r.err, "no HCSInfer server reachable")) << r.err;
    EXPECT_TRUE(contains(r.err, "HCSInfer serve"));
    auto l = run({"models", "load", "tiny", "--port", std::to_string(dead_port), "--token", kToken});
    EXPECT_EQ(l.code, 8);
    EXPECT_TRUE(contains(l.err, "no HCSInfer server reachable"));
}

TEST(CliRemote, LoadStatusUnloadRoundTrip) {
    Server s;
    auto l = run(s.conn({"models", "load", "tiny"}));
    ASSERT_EQ(l.code, 0) << l.err;
    EXPECT_TRUE(contains(l.out, "loaded tiny"));
    EXPECT_EQ(s.fx.engine.loads.load(), 1);
    ASSERT_TRUE(s.fx.cp->loaded_model().has_value());
    EXPECT_EQ(*s.fx.cp->loaded_model(), "tiny");

    auto st = run(s.conn({"models", "status", "--json"}));
    ASSERT_EQ(st.code, 0) << st.err;
    auto j = json::parse(st.out);
    EXPECT_EQ(j["loaded_model"], "tiny");
    EXPECT_EQ(j["server"]["health"], "ok");

    auto u = run(s.conn({"models", "unload"}));  // no id: unloads whatever is loaded
    ASSERT_EQ(u.code, 0) << u.err;
    EXPECT_TRUE(contains(u.out, "unloaded tiny"));
    EXPECT_FALSE(s.fx.cp->loaded_model().has_value());
    EXPECT_EQ(s.fx.engine.unloads.load(), 1);

    auto again = run(s.conn({"models", "unload"}));
    EXPECT_EQ(again.code, exit_code(ErrorCode::InvalidState));
    EXPECT_TRUE(contains(again.err, "no model is loaded"));
}

TEST(CliRemote, ExplicitUnloadOfTheWrongModelIsInvalidState) {
    Server s;
    ASSERT_EQ(run(s.conn({"models", "load", "tiny"})).code, 0);
    auto r = run(s.conn({"models", "unload", "other"}));
    EXPECT_EQ(r.code, exit_code(ErrorCode::InvalidState));
    EXPECT_TRUE(s.fx.cp->loaded_model().has_value());
}

TEST(CliRemote, UnknownModelIsNotFoundAndMalformedIdNeverReachesTheWire) {
    Server s;
    EXPECT_EQ(run(s.conn({"models", "load", "ghost"})).code, exit_code(ErrorCode::NotFound));
    auto bad = run(s.conn({"models", "load", "../etc/passwd"}));
    EXPECT_EQ(bad.code, exit_code(ErrorCode::InvalidArgument));
    EXPECT_EQ(run(s.conn({"models", "load"})).code, 2);
    EXPECT_EQ(run(s.conn({"models", "load", "a", "b"})).code, 2);
    EXPECT_EQ(run(s.conn({"models", "status", "extra"})).code, 2);
}

TEST(CliRemote, TokenIsRequiredAndCheckedAndNeverEchoed) {
    Server s;
    auto none = run(s.conn({"models", "load", "tiny"}, false));
    EXPECT_EQ(none.code, exit_code(ErrorCode::InvalidArgument));
    EXPECT_TRUE(contains(none.err, "HCS_API_TOKEN"));
    auto wrong = run(
        {"models", "load", "tiny", "--port", std::to_string(s.port), "--token", "wrong-token-wrong-token"});
    EXPECT_EQ(wrong.code, exit_code(ErrorCode::SecurityViolation));
    EXPECT_FALSE(contains(wrong.err, "wrong-token-wrong-token"));
    EXPECT_FALSE(contains(wrong.out, kToken));
    EXPECT_FALSE(s.fx.cp->loaded_model().has_value());
}

TEST(CliRemote, StatusWithoutTokenReportsHealthOnly) {
    Server s;
    auto r = run(s.conn({"models", "status"}, false));
    ASSERT_EQ(r.code, 0) << r.err;
    EXPECT_TRUE(contains(r.out, "(ok)"));
    EXPECT_TRUE(contains(r.out, "no API token given"));
}

TEST(CliRemote, EngineFailureExitCodeIsTheApisErrorCode) {
    Server s;
    s.fx.engine.fail_load = true;
    s.fx.engine.fail_load_code = ErrorCode::OutOfMemory;
    auto r = run(s.conn({"models", "load", "tiny"}));
    EXPECT_EQ(r.code, exit_code(ErrorCode::OutOfMemory));
    EXPECT_TRUE(contains(r.err, "[out_of_memory]"));
    EXPECT_FALSE(s.fx.cp->loaded_model().has_value());
}

TEST(CliRemote, NoWaitReturnsTheJobIdWhileTheLoadRuns) {
    Server s;
    s.fx.engine.load_delay_ms = 300;
    auto r = run(s.conn({"models", "load", "tiny", "--no-wait", "--json"}));
    ASSERT_EQ(r.code, 0) << r.err;
    auto j = json::parse(r.out);
    EXPECT_EQ(j["id"], "tiny");
    EXPECT_EQ(j["job_id"].get<std::string>().rfind("job-", 0), 0u);
}

TEST(CliRemote, TimeoutWhileLoadingIsExit14AndTheJobKeepsRunning) {
    Server s;
    s.fx.engine.load_delay_ms = 2500;
    auto r = run(s.conn({"models", "load", "tiny", "--timeout-s", "1"}));
    EXPECT_EQ(r.code, exit_code(ErrorCode::Timeout));
    EXPECT_TRUE(contains(r.err, "still"));
    // the server-side job is not cancelled by a CLI timeout: it finishes and the model ends up loaded
    for (int i = 0; i < 100 && !s.fx.cp->loaded_model(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_TRUE(s.fx.cp->loaded_model().has_value());
}

TEST(CliRemote, OptionValidation) {
    EXPECT_EQ(run({"models", "status", "--port", "0"}).code, 2);
    EXPECT_EQ(run({"models", "status", "--port", "99999"}).code, 2);
    EXPECT_EQ(run({"models", "status", "--port"}).code, 2);
    EXPECT_EQ(run({"models", "load", "x", "--timeout-s", "0"}).code, 2);
    EXPECT_EQ(run({"models", "status", "--host"}).code, 2);
}
