#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hcs/agent/agent_config.hpp"
#include "hcs/path_guard.hpp"
#include "hcs/result.hpp"
#include "temp_dir.hpp"

using namespace hcs;
using namespace hcs::agent;
using hcs::test::TempDir;
using nlohmann::json;

namespace {

ProviderConfig valid_provider() {
    ProviderConfig p;
    p.name = "hcs";
    p.base_url = "http://127.0.0.1:8765/v1";
    p.api_key = "run-token-abc123";
    p.models = {{"lfm3-omni-2b", 8192, 2048, {"text", "image"}}};
    return p;
}

LaunchRequest valid_request(const std::filesystem::path& workspace) {
    LaunchRequest r;
    r.program = workspace / "prime-agent.exe";
    if (auto added = r.workspaces.add_root(workspace); !added) {
        throw std::runtime_error("add_root failed: " + added.error().message);
    }
    r.workspace = workspace / "work";
    std::filesystem::create_directories(r.workspace);
    r.agent_dir = workspace / "agent";
    r.provider = valid_provider();
    r.model_id = "lfm3-omni-2b";
    return r;
}

bool args_contain(const std::vector<std::string>& args, const std::string& a) {
    return std::find(args.begin(), args.end(), a) != args.end();
}

}  // namespace

TEST(AgentConfig, LoopbackBaseUrlAcceptsOnlyLoopbackV1) {
    EXPECT_TRUE(is_loopback_base_url("http://127.0.0.1:8765/v1"));
    EXPECT_TRUE(is_loopback_base_url("http://127.0.0.1/v1"));
    EXPECT_TRUE(is_loopback_base_url("http://localhost:1234/v1/"));
    EXPECT_TRUE(is_loopback_base_url("http://[::1]:8765/v1"));

    EXPECT_FALSE(is_loopback_base_url("https://127.0.0.1:8765/v1"));  // http only
    EXPECT_FALSE(is_loopback_base_url("http://example.com/v1"));
    EXPECT_FALSE(is_loopback_base_url("http://0.0.0.0:8765/v1"));
    EXPECT_FALSE(is_loopback_base_url("http://127.0.0.1:8765/v2"));
    EXPECT_FALSE(is_loopback_base_url("http://127.0.0.1:8765"));
    EXPECT_FALSE(is_loopback_base_url("http://127.0.0.1:0/v1"));
    EXPECT_FALSE(is_loopback_base_url("http://127.0.0.1:99999/v1"));
    EXPECT_FALSE(is_loopback_base_url("http://user@127.0.0.1:8765/v1"));
    EXPECT_FALSE(is_loopback_base_url("http://127.0.0.1:8765/v1?x=1"));
    EXPECT_FALSE(is_loopback_base_url(""));
}

TEST(AgentConfig, ProviderJsonHasTheExpectedShape) {
    auto r = provider_config_json(valid_provider());
    ASSERT_TRUE(r) << r.error().message;
    const json j = json::parse(r.value());
    ASSERT_TRUE(j.contains("providers"));
    const json& p = j["providers"]["hcs"];
    EXPECT_EQ(p["baseUrl"], "http://127.0.0.1:8765/v1");
    EXPECT_EQ(p["apiKey"], "run-token-abc123");
    EXPECT_EQ(p["api"], "openai-completions");
    EXPECT_EQ(p["authHeader"], true);
    ASSERT_EQ(p["models"].size(), 1u);
    EXPECT_EQ(p["models"][0]["id"], "lfm3-omni-2b");
    EXPECT_EQ(p["models"][0]["contextWindow"], 8192);
    ASSERT_EQ(p["models"][0]["input"].size(), 2u);
}

TEST(AgentConfig, ProviderRejectsUnsafeFields) {
    auto bad = [](ProviderConfig p) {
        auto r = provider_config_json(p);
        EXPECT_FALSE(r) << "expected rejection";
        if (!r)
            EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    };
    {
        ProviderConfig p = valid_provider();
        p.base_url = "http://example.com/v1";
        bad(p);
    }
    {
        ProviderConfig p = valid_provider();
        p.base_url = "http://127.0.0.1:8765";
        bad(p);
    }
    {
        ProviderConfig p = valid_provider();
        p.api_key = "";
        bad(p);
    }
    {
        ProviderConfig p = valid_provider();
        p.api_key = "has space";
        bad(p);
    }
    {
        ProviderConfig p = valid_provider();
        p.api = "anthropic";
        bad(p);
    }
    {
        ProviderConfig p = valid_provider();
        p.models.clear();
        bad(p);
    }
    {
        ProviderConfig p = valid_provider();
        p.models[0].id = "bad id";
        bad(p);
    }
    {
        ProviderConfig p = valid_provider();
        p.models[0].context_window = 10;
        bad(p);
    }
    {
        ProviderConfig p = valid_provider();
        p.models[0].max_tokens = 100000;
        bad(p);
    }
    {
        ProviderConfig p = valid_provider();
        p.models[0].input = {"audio"};
        bad(p);
    }
    {
        ProviderConfig p = valid_provider();
        p.name = "not an id";
        bad(p);
    }
}

TEST(AgentConfig, LaunchPlanIsOfflineAndScoped) {
    TempDir t;
    auto r = build_launch_plan(valid_request(t.path()));
    ASSERT_TRUE(r) << r.error().message;
    const LaunchPlan& plan = r.value();
    ASSERT_FALSE(plan.args.empty());
    EXPECT_EQ(plan.args.front(), "--offline");
    EXPECT_TRUE(args_contain(plan.args, "--provider"));
    EXPECT_TRUE(args_contain(plan.args, "hcs"));
    EXPECT_TRUE(args_contain(plan.args, "--model"));
    EXPECT_TRUE(args_contain(plan.args, "lfm3-omni-2b"));
    EXPECT_TRUE(args_contain(plan.args, "--api-key"));
    EXPECT_EQ(plan.env.at("PI_OFFLINE"), "1");
    EXPECT_FALSE(plan.env.at("PRIME_AGENT_CODING_AGENT_DIR").empty());
    EXPECT_TRUE(plan.cwd.is_absolute());
    // cwd is the canonicalised workspace, inside the approved root.
    EXPECT_EQ(plan.cwd.filename().string(), "work");
}

TEST(AgentConfig, LaunchPlanRejectsUnsafeInputs) {
    TempDir t;
    {
        LaunchRequest r = valid_request(t.path());
        r.program = "prime-agent.exe";  // relative
        EXPECT_FALSE(build_launch_plan(r));
    }
    {
        LaunchRequest r = valid_request(t.path());
        r.agent_dir = "relative/agent";
        EXPECT_FALSE(build_launch_plan(r));
    }
    {
        LaunchRequest r = valid_request(t.path());
        r.workspaces = PathGuard{};
        EXPECT_FALSE(build_launch_plan(r));
    }
    {
        LaunchRequest r = valid_request(t.path());
        r.model_id = "not-listed";
        auto rr = build_launch_plan(r);
        ASSERT_FALSE(rr);
        EXPECT_EQ(rr.error().code, ErrorCode::InvalidArgument);
    }
    {
        // A workspace outside the approved root is refused.
        TempDir other;
        LaunchRequest r = valid_request(t.path());
        r.workspace = other.path();
        auto rr = build_launch_plan(r);
        ASSERT_FALSE(rr);
        EXPECT_EQ(rr.error().code, ErrorCode::SecurityViolation);
    }
    {
        LaunchRequest r = valid_request(t.path());
        r.provider.base_url = "http://192.168.0.5:8765/v1";  // non-loopback
        EXPECT_FALSE(build_launch_plan(r));
    }
}
