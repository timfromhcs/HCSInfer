// Contract tests for the MCP stdio server. FakeTextEngine is used for plumbing only (not inference).
#include <gtest/gtest.h>

#include <condition_variable>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include "api_fixture.hpp"
#include "hcs/mcp/mcp_server.hpp"
#include "test_png.hpp"

using namespace hcs;
using namespace hcs::mcp;
using nlohmann::json;

namespace {

json meta() {
    return {{"io.modelcontextprotocol/protocolVersion", kModernVersion},
            {"io.modelcontextprotocol/clientInfo", {{"name", "test"}, {"version", "0"}}},
            {"io.modelcontextprotocol/clientCapabilities", json::object()}};
}

json modern_req(int id, const std::string& method, json params = json::object()) {
    params["_meta"] = meta();
    return {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
}

json call_req(int id, const std::string& tool, json args = json::object()) {
    return modern_req(id, "tools/call", {{"name", tool}, {"arguments", args}});
}

struct Mcp {
    test::ApiFixture fx;
    McpServer server;
    explicit Mcp(McpOptions o = {}, bool with_engine = true)
        : fx(with_engine), server(*fx.cp, o, fx.logger) {}
    json rpc(const json& req) {
        const std::string line = server.handle_line(req.dump());
        return line.empty() ? json(nullptr) : json::parse(line);
    }
    json tool(int id, const std::string& name, json args = json::object()) {
        return rpc(call_req(id, name, args));
    }
};

bool wait_for(const std::function<bool()>& f, int ms = 3000) {
    for (int i = 0; i < ms / 5; ++i) {
        if (f())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return f();
}

// Blocking in-memory pipe used as the server's "stdin".
class PipeBuf : public std::streambuf {
public:
    void feed(const std::string& s) {
        {
            std::lock_guard lk(m_);
            data_ += s;
        }
        cv_.notify_all();
    }
    void close() {
        {
            std::lock_guard lk(m_);
            closed_ = true;
        }
        cv_.notify_all();
    }

protected:
    int_type underflow() override {
        std::unique_lock lk(m_);
        cv_.wait(lk, [&] { return pos_ < data_.size() || closed_; });
        if (pos_ >= data_.size())
            return traits_type::eof();
        cur_ = data_[pos_++];
        setg(&cur_, &cur_, &cur_ + 1);
        return traits_type::to_int_type(cur_);
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::string data_;
    std::size_t pos_ = 0;
    bool closed_ = false;
    char cur_ = 0;
};

// Thread-safe output capture so tests can poll while run() is active.
class OutBuf : public std::streambuf {
public:
    std::vector<std::string> lines() {
        std::lock_guard lk(m_);
        std::vector<std::string> out;
        std::istringstream is(data_);
        std::string l;
        while (std::getline(is, l)) out.push_back(l);
        return out;
    }

protected:
    int_type overflow(int_type c) override {
        if (c != traits_type::eof()) {
            std::lock_guard lk(m_);
            data_.push_back(static_cast<char>(c));
        }
        return traits_type::not_eof(c);
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        std::lock_guard lk(m_);
        data_.append(s, static_cast<std::size_t>(n));
        return n;
    }

private:
    std::mutex m_;
    std::string data_;
};

}  // namespace

// ---- protocol / era handling ---------------------------------------------------------------------------
TEST(McpProtocol, ServerDiscoverAdvertisesVersionsAndTools) {
    Mcp m;
    auto r = m.rpc(modern_req(1, "server/discover"));
    ASSERT_TRUE(r.contains("result")) << r.dump();
    EXPECT_EQ(r["result"]["resultType"], "complete");
    EXPECT_EQ(r["result"]["supportedVersions"][0], kModernVersion);
    EXPECT_TRUE(r["result"]["capabilities"].contains("tools"));
    EXPECT_EQ(r["result"]["serverInfo"]["name"], "hcsinfer");
    EXPECT_FALSE(r["result"]["capabilities"].contains("resources"));
}

TEST(McpProtocol, DiscoverRejectsUnknownRequestedVersionWithSupportedList) {
    Mcp m;
    auto req = modern_req(1, "server/discover");
    req["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"] = "1900-01-01";
    auto r = m.rpc(req);
    ASSERT_TRUE(r.contains("error"));
    EXPECT_EQ(r["error"]["code"], -32022);
    EXPECT_EQ(r["error"]["data"]["requested"], "1900-01-01");
    EXPECT_TRUE(r["error"]["data"]["supported"].is_array());
}

TEST(McpProtocol, RequestsMustCarryMetaVersionAndCapabilities) {
    Mcp m;
    auto no_meta = m.rpc({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}});
    EXPECT_EQ(no_meta["error"]["code"], -32602);
    auto bad_ver = modern_req(2, "tools/list");
    bad_ver["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"] = "2099-01-01";
    EXPECT_EQ(m.rpc(bad_ver)["error"]["code"], -32022);
    auto no_caps = modern_req(3, "tools/list");
    no_caps["params"]["_meta"].erase("io.modelcontextprotocol/clientCapabilities");
    EXPECT_EQ(m.rpc(no_caps)["error"]["code"], -32602);
}

TEST(McpProtocol, ModernResultsCarryResultTypeAndServerInfo) {
    Mcp m;
    auto r = m.rpc(modern_req(1, "tools/list"));
    ASSERT_TRUE(r.contains("result")) << r.dump();
    EXPECT_EQ(r["result"]["resultType"], "complete");
    EXPECT_TRUE(r["result"].contains("ttlMs"));
    EXPECT_EQ(r["result"]["cacheScope"], "public");
    EXPECT_EQ(r["result"]["_meta"]["io.modelcontextprotocol/serverInfo"]["name"], "hcsinfer");
    EXPECT_EQ(r["jsonrpc"], "2.0");
    EXPECT_EQ(r["id"], 1);
}

TEST(McpProtocol, PingIsRemovedInModernEra) {
    Mcp m;
    EXPECT_EQ(m.rpc(modern_req(1, "ping"))["error"]["code"], -32601);
    EXPECT_EQ(m.rpc(modern_req(2, "no/such/method"))["error"]["code"], -32601);
}

TEST(McpProtocol, LegacyHandshakeStillWorks) {
    Mcp m;
    // Before initialize a bare request is refused with a hint.
    EXPECT_EQ(m.rpc({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}})["error"]["code"], -32602);
    auto init = m.rpc({{"jsonrpc", "2.0"},
                       {"id", 2},
                       {"method", "initialize"},
                       {"params",
                        {{"protocolVersion", kLegacyVersion},
                         {"capabilities", json::object()},
                         {"clientInfo", {{"name", "old"}, {"version", "1"}}}}}});
    ASSERT_TRUE(init.contains("result")) << init.dump();
    EXPECT_EQ(init["result"]["protocolVersion"], kLegacyVersion);
    EXPECT_TRUE(init["result"]["capabilities"].contains("tools"));
    EXPECT_FALSE(init["result"].contains("resultType"));
    // Not usable until notifications/initialized
    EXPECT_TRUE(m.rpc({{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/list"}}).contains("error"));
    EXPECT_TRUE(m.rpc({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}})
                    .is_null());  // notification: no reply
    auto ping = m.rpc({{"jsonrpc", "2.0"}, {"id", 4}, {"method", "ping"}});
    EXPECT_TRUE(ping["result"].is_object());
    auto list = m.rpc({{"jsonrpc", "2.0"}, {"id", 5}, {"method", "tools/list"}});
    ASSERT_TRUE(list.contains("result"));
    EXPECT_GE(list["result"]["tools"].size(), 11u);
    auto call = m.rpc({{"jsonrpc", "2.0"},
                       {"id", 6},
                       {"method", "tools/call"},
                       {"params", {{"name", "hcs.models.list"}, {"arguments", json::object()}}}});
    EXPECT_FALSE(call["result"]["isError"].get<bool>());
}

TEST(McpProtocol, MalformedMessages) {
    Mcp m;
    auto parse = json::parse(m.server.handle_line("{not json"));
    EXPECT_EQ(parse["error"]["code"], -32700);
    EXPECT_TRUE(parse["id"].is_null());
    EXPECT_EQ(json::parse(m.server.handle_line("[]"))["error"]["code"], -32600);  // batches unsupported
    EXPECT_EQ(json::parse(m.server.handle_line(R"({"id":1,"method":"x"})"))["error"]["code"],
              -32600);  // no jsonrpc
    EXPECT_EQ(
        json::parse(m.server.handle_line(R"({"jsonrpc":"2.0","id":{"a":1},"method":"x"})"))["error"]["code"],
        -32600);
    EXPECT_EQ(json::parse(m.server.handle_line(R"({"jsonrpc":"2.0","id":1,"method":5})"))["error"]["code"],
              -32600);
    EXPECT_EQ(json::parse(m.server.handle_line(
                  R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":[]})"))["error"]["code"],
              -32602);
    EXPECT_TRUE(m.server.handle_line(R"({"jsonrpc":"2.0","method":"notifications/unknown"})").empty());
    EXPECT_TRUE(
        m.server
            .handle_line(R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":999}})")
            .empty());
}

// ---- tools
// --------------------------------------------------------------------------------------------------
TEST(McpTools, ListHasNarrowSchemasForEveryRequiredTool) {
    Mcp m;
    auto r = m.rpc(modern_req(1, "tools/list"));
    std::set<std::string> names;
    for (const auto& t : r["result"]["tools"]) {
        const std::string n = t["name"];
        names.insert(n);
        EXPECT_LE(n.size(), 128u);
        for (char c : n)
            EXPECT_TRUE(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.') << n;
        EXPECT_EQ(t["inputSchema"]["type"], "object") << n;
        EXPECT_EQ(t["inputSchema"]["additionalProperties"], false) << "schema for " << n << " must be closed";
        EXPECT_TRUE(t.contains("description"));
        EXPECT_TRUE(t["annotations"].contains("readOnlyHint"));
    }
    for (const char* want : {"hcs.models.list", "hcs.models.inspect", "hcs.models.load", "hcs.models.unload",
                             "hcs.chat.generate", "hcs.runtime.status", "hcs.runtime.benchmark",
                             "hcs.jobs.status", "hcs.jobs.cancel", "hcs.vision.analyze", "hcs.image.generate",
                             "hcs.image.edit", "hcs.runtime.shed", "hcs.loras.list"})
        EXPECT_TRUE(names.count(want)) << want;
    // deterministic order across calls
    EXPECT_EQ(m.rpc(modern_req(2, "tools/list"))["result"]["tools"], r["result"]["tools"]);
    // No arbitrary shell / filesystem tools.
    for (const auto& n : names) {
        EXPECT_EQ(n.find("shell"), std::string::npos);
        EXPECT_EQ(n.find("exec"), std::string::npos);
        EXPECT_EQ(n.find("file"), std::string::npos);
    }
}

TEST(McpTools, ModelsListAndInspect) {
    Mcp m;
    auto l = m.tool(1, "hcs.models.list");
    ASSERT_FALSE(l["result"]["isError"].get<bool>()) << l.dump();
    EXPECT_EQ(l["result"]["structuredContent"]["models"].size(), 2u);
    EXPECT_EQ(l["result"]["content"][0]["type"], "text");
    auto i = m.tool(2, "hcs.models.inspect", {{"id", "tiny"}});
    EXPECT_EQ(i["result"]["structuredContent"]["id"], "tiny");
    EXPECT_FALSE(i["result"]["structuredContent"].contains("path"));
    auto missing = m.tool(3, "hcs.models.inspect", {{"id", "nope"}});
    EXPECT_TRUE(missing["result"]["isError"].get<bool>());
    EXPECT_EQ(missing["result"]["structuredContent"]["error"]["code"], "not_found");
}

TEST(McpTools, InvalidArgumentsAreToolErrorsUnknownToolIsProtocolError) {
    Mcp m;
    for (const json& args : {json{{"id", 5}}, json{{"id", "tiny"}, {"extra", 1}}, json::object(),
                             json{{"id", std::string(100, 'a')}}}) {
        auto r = m.tool(1, "hcs.models.inspect", args);
        ASSERT_TRUE(r.contains("result")) << args.dump() << " -> " << r.dump();
        EXPECT_TRUE(r["result"]["isError"].get<bool>()) << args.dump();
        EXPECT_EQ(r["result"]["structuredContent"]["error"]["code"], "invalid_argument");
    }
    auto unknown = m.tool(2, "hcs.rm.rf");
    EXPECT_EQ(unknown["error"]["code"], -32602);
    EXPECT_EQ(m.rpc(modern_req(3, "tools/call", {{"name", 5}}))["error"]["code"], -32602);
    EXPECT_EQ(m.rpc(modern_req(4, "tools/call",
                               {{"name", "hcs.models.list"}, {"arguments", "x"}}))["error"]["code"],
              -32602);
}

TEST(McpTools, LoadChatUnloadRoundTrip) {
    Mcp m;
    auto load = m.tool(1, "hcs.models.load", {{"id", "tiny"}});
    ASSERT_FALSE(load["result"]["isError"].get<bool>()) << load.dump();
    EXPECT_EQ(m.fx.engine.loads.load(), 1);
    auto chat = m.tool(2, "hcs.chat.generate", {{"prompt", "hello"}, {"max_tokens", 16}});
    ASSERT_FALSE(chat["result"]["isError"].get<bool>()) << chat.dump();
    EXPECT_EQ(chat["result"]["structuredContent"]["text"], "Hello from the fake engine");
    EXPECT_EQ(chat["result"]["structuredContent"]["finish_reason"], "stop");
    EXPECT_EQ(chat["result"]["structuredContent"]["model"], "tiny");
    auto st = m.tool(3, "hcs.jobs.status", {{"job_id", chat["result"]["structuredContent"]["job_id"]}});
    EXPECT_EQ(st["result"]["structuredContent"]["state"], "succeeded");
    auto rt = m.tool(4, "hcs.runtime.status");
    EXPECT_EQ(rt["result"]["structuredContent"]["runtime"]["loaded_model"], "tiny");
    EXPECT_FALSE(rt["result"]["structuredContent"].contains("devices"));
    EXPECT_FALSE(m.tool(5, "hcs.models.unload")["result"]["isError"].get<bool>());
    EXPECT_TRUE(m.tool(6, "hcs.models.unload")["result"]["isError"].get<bool>());
    // chat with a model argument auto-loads
    auto chat2 =
        m.tool(7, "hcs.chat.generate",
               {{"model", "other"}, {"messages", json::array({json{{"role", "user"}, {"content", "x"}}})}});
    EXPECT_FALSE(chat2["result"]["isError"].get<bool>()) << chat2.dump();
}

TEST(McpTools, ChatNeedsExactlyOneOfPromptOrMessages) {
    Mcp m;
    EXPECT_TRUE(m.tool(1, "hcs.chat.generate", {{"model", "tiny"}})["result"]["isError"].get<bool>());
    EXPECT_TRUE(
        m.tool(2, "hcs.chat.generate",
               {{"model", "tiny"},
                {"prompt", "a"},
                {"messages", json::array({json{{"role", "user"}, {"content", "b"}}})}})["result"]["isError"]
            .get<bool>());
    EXPECT_TRUE(m.tool(3, "hcs.chat.generate",
                       {{"model", "tiny"}, {"prompt", "a"}, {"max_tokens", 100000000}})["result"]["isError"]
                    .get<bool>());
}

TEST(McpTools, ModelIdPathTraversalIsRejected) {
    Mcp m;
    for (const char* id :
         {"../../etc/passwd", "..\\..\\windows\\system32", "C:\\windows\\win.ini", "tiny/../other"}) {
        auto r = m.tool(1, "hcs.models.load", {{"id", id}});
        EXPECT_TRUE(r["result"]["isError"].get<bool>()) << id;
        EXPECT_EQ(r["result"]["structuredContent"]["error"]["code"], "invalid_argument") << id;
    }
    EXPECT_EQ(m.fx.engine.loads.load(), 0);
}

TEST(McpTools, ImagePathsAreGuardedEvenForUnsupportedTools) {
    Mcp m;
    test::TempDir outside;
    test::write_file(outside / "secret.png", "x");
    auto esc = m.tool(1, "hcs.vision.analyze",
                      {{"prompt", "what"}, {"image_path", (outside / "secret.png").string()}});
    EXPECT_TRUE(esc["result"]["isError"].get<bool>());
    EXPECT_EQ(esc["result"]["structuredContent"]["error"]["code"], "security_violation");
    auto dots = m.tool(2, "hcs.vision.analyze",
                       {{"prompt", "what"}, {"image_path", (m.fx.root / ".." / "x.png").string()}});
    EXPECT_EQ(dots["result"]["structuredContent"]["error"]["code"], "security_violation");
    {
        // a real (generated) PNG: the fake engine has no vision, so the call reaches the engine and is
        // "unsupported"
        const auto png = test::make_red_square_png(32);
        test::write_file(m.fx.root / "ok.png", std::string(png.begin(), png.end()));
    }
    auto ok = m.tool(3, "hcs.vision.analyze",
                     {{"prompt", "what"}, {"image_path", (m.fx.root / "ok.png").string()}});
    EXPECT_EQ(ok["result"]["structuredContent"]["error"]["code"], "unsupported");
    EXPECT_EQ(m.tool(4, "hcs.image.generate",
                     {{"prompt", "a cat"}})["result"]["structuredContent"]["error"]["code"],
              "unsupported");
    EXPECT_EQ(m.tool(5, "hcs.image.edit",
                     {{"prompt", "a cat"},
                      {"input_path",
                       (outside / "secret.png").string()}})["result"]["structuredContent"]["error"]["code"],
              "security_violation");
}

TEST(McpTools, JobIdsAreValidated) {
    Mcp m;
    EXPECT_EQ(
        m.tool(1, "hcs.jobs.status", {{"job_id", "banana"}})["result"]["structuredContent"]["error"]["code"],
        "invalid_argument");
    EXPECT_EQ(
        m.tool(2, "hcs.jobs.status", {{"job_id", "job-777"}})["result"]["structuredContent"]["error"]["code"],
        "not_found");
    EXPECT_EQ(
        m.tool(3, "hcs.jobs.cancel", {{"job_id", "job-777"}})["result"]["structuredContent"]["error"]["code"],
        "not_found");
}

TEST(McpTools, NoEngineIsReportedAsUnsupported) {
    Mcp m({}, false);
    EXPECT_EQ(m.tool(1, "hcs.models.load", {{"id", "tiny"}})["result"]["structuredContent"]["error"]["code"],
              "unsupported");
    EXPECT_EQ(
        m.tool(2, "hcs.chat.generate", {{"prompt", "x"}})["result"]["structuredContent"]["error"]["code"],
        "unsupported");
}

// ---- hcs.runtime.benchmark (plumbing only: FakeTextEngine numbers are not benchmark evidence,
// AGENTS.md 16.2)
TEST(McpTools, BenchmarkQuickSuiteReturnsTheAgentsDocument) {
    Mcp m;
    auto r =
        m.tool(1, "hcs.runtime.benchmark",
               {{"model", "tiny"}, {"suite", "quick"}, {"warmup", 0}, {"iterations", 1}, {"max_tokens", 8}});
    ASSERT_FALSE(r["result"]["isError"].get<bool>()) << r.dump();
    const json& doc = r["result"]["structuredContent"];
    EXPECT_EQ(doc["schema_version"], 1);
    EXPECT_EQ(doc["suite"], "quick");
    EXPECT_EQ(doc["result_status"], "PASS");
    EXPECT_EQ(doc["model_file"], "tiny.gguf");
    EXPECT_EQ(doc["measured_iterations"], 1);
    ASSERT_TRUE(doc["samples"]["ttft_ms"]["samples"].is_array());
    EXPECT_EQ(doc["samples"]["ttft_ms"]["samples"].size(), 1u);
    // Cold load + one warm reload through the control plane; the benchmarked model is left loaded.
    EXPECT_EQ(m.fx.engine.loads.load(), 2);
    EXPECT_EQ(m.fx.cp->loaded_model().value_or(""), "tiny");
}

TEST(McpTools, BenchmarkFullSuiteReturnsSweepsAndSweepSizesRequireFull) {
    Mcp m;
    auto full = m.tool(1, "hcs.runtime.benchmark",
                       {{"model", "tiny"},
                        {"suite", "full"},
                        {"sweep_sizes", json::array({16, 32})},
                        {"warmup", 0},
                        {"iterations", 1},
                        {"max_tokens", 8}});
    ASSERT_FALSE(full["result"]["isError"].get<bool>()) << full.dump();
    const json& doc = full["result"]["structuredContent"];
    EXPECT_EQ(doc["suite"], "full");
    ASSERT_EQ(doc["prefill_sweep"].size(), 2u);
    ASSERT_EQ(doc["decode_sweep"].size(), 2u);
    EXPECT_EQ(doc["prefill_sweep"][0]["target_prompt_tokens"], 16);
    // sweep_sizes is only meaningful for the full suite.
    auto bad = m.tool(2, "hcs.runtime.benchmark",
                      {{"model", "tiny"}, {"suite", "quick"}, {"sweep_sizes", json::array({16})}});
    EXPECT_TRUE(bad["result"]["isError"].get<bool>());
    EXPECT_EQ(bad["result"]["structuredContent"]["error"]["code"], "invalid_argument");
}

TEST(McpTools, BenchmarkValidationAndNoEngineAreHonestErrors) {
    Mcp m;
    EXPECT_EQ(m.tool(1, "hcs.runtime.benchmark",
                     {{"model", "../x"}})["result"]["structuredContent"]["error"]["code"],
              "invalid_argument");
    EXPECT_EQ(m.tool(2, "hcs.runtime.benchmark",
                     {{"model", "nope"}})["result"]["structuredContent"]["error"]["code"],
              "not_found");
    EXPECT_EQ(m.tool(3, "hcs.runtime.benchmark",
                     {{"model", "tiny"}, {"suite", "wrong"}})["result"]["structuredContent"]["error"]["code"],
              "invalid_argument");
    Mcp none({}, false);
    EXPECT_EQ(none.tool(4, "hcs.runtime.benchmark",
                        {{"model", "tiny"}})["result"]["structuredContent"]["error"]["code"],
              "unsupported");
}

TEST(McpTools, BenchmarkCallTimeoutCancelsTheUnderlyingJob) {
    McpOptions o;
    o.call_timeout = std::chrono::milliseconds(120);
    Mcp m(o);
    m.fx.engine.delay_ms = 30;  // ~150 ms per generation; the whole suite cannot finish in 120 ms
    auto r = m.tool(1, "hcs.runtime.benchmark",
                    {{"model", "tiny"}, {"warmup", 5}, {"iterations", 10}, {"max_tokens", 64}});
    EXPECT_TRUE(r["result"]["isError"].get<bool>());
    EXPECT_EQ(r["result"]["structuredContent"]["error"]["code"], "timeout");
    EXPECT_TRUE(wait_for([&] {
        auto st = m.fx.cp->job_status(JobId{1});
        return st && (st.value().state == JobState::Cancelled || st.value().state == JobState::Succeeded);
    })) << "the benchmark job was left running after the tool timed out";
}

// ---- stdio loop
// ----------------------------------------------------------------------------------------------
TEST(McpStdio, StdoutCarriesOnlyJsonRpcAndLogsStayElsewhere) {
    Mcp m;
    std::stringstream in;
    in << modern_req(1, "server/discover").dump() << "\n"
       << "garbage that is not json\n"
       << "\n"
       << modern_req(2, "tools/list").dump() << "\n"
       << call_req(3, "hcs.models.load", {{"id", "tiny"}}).dump() << "\n"
       << call_req(4, "hcs.chat.generate", {{"prompt", "hi"}}).dump() << "\n"
       << json({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}).dump() << "\n";
    std::ostringstream out;
    m.server.run(in, out);
    std::istringstream lines(out.str());
    std::string l;
    int n = 0;
    std::set<int> answered;
    while (std::getline(lines, l)) {
        ASSERT_FALSE(l.empty());
        auto j = json::parse(l, nullptr, false);
        ASSERT_FALSE(j.is_discarded()) << "non-JSON line on stdout: " << l;
        EXPECT_EQ(j["jsonrpc"], "2.0");
        EXPECT_TRUE(j.contains("result") || j.contains("error"));
        if (j["id"].is_number())
            answered.insert(j["id"].get<int>());
        ++n;
    }
    EXPECT_EQ(n, 5);  // discover, parse error, list, load, chat
    EXPECT_EQ(answered, (std::set<int>{1, 2, 3, 4}));
    EXPECT_EQ(out.str().find("\"level\""), std::string::npos) << "log records leaked onto stdout";
}

TEST(McpStdio, OversizeLineIsRejectedNotBuffered) {
    McpOptions o;
    o.max_line_bytes = 256;
    Mcp m(o);
    std::stringstream in;
    in << std::string(100000, 'a') << "\n" << modern_req(2, "server/discover").dump() << "\n";
    std::ostringstream out;
    m.server.run(in, out);
    std::istringstream lines(out.str());
    std::string l;
    std::getline(lines, l);
    EXPECT_EQ(json::parse(l)["error"]["code"], -32600);
    std::getline(lines, l);
    EXPECT_EQ(json::parse(l)["id"], 2);  // the stream resynchronised on the next line
}

TEST(McpStdio, BatchesAreRejected) {
    Mcp m;
    std::stringstream in;
    in << R"([{"jsonrpc":"2.0","id":1,"method":"tools/list"}])" << "\n";
    std::ostringstream out;
    m.server.run(in, out);
    EXPECT_EQ(json::parse(out.str())["error"]["code"], -32600);
}

TEST(McpStdio, CancelNotificationStopsWorkAndSuppressesTheResponse) {
    Mcp m;
    m.fx.engine.run_until_cancel = true;
    m.fx.engine.delay_ms = 2;
    PipeBuf pipe;
    std::istream in(&pipe);
    OutBuf outbuf;
    std::ostream out(&outbuf);
    std::thread srv([&] { m.server.run(in, out); });

    pipe.feed(call_req(1, "hcs.models.load", {{"id", "tiny"}}).dump() + "\n");
    ASSERT_TRUE(wait_for([&] { return outbuf.lines().size() >= 1; }));
    pipe.feed(call_req(7, "hcs.chat.generate", {{"prompt", "go"}, {"max_tokens", 100}}).dump() + "\n");
    ASSERT_TRUE(wait_for([&] { return m.fx.engine.generating.load(); }));
    pipe.feed(json({{"jsonrpc", "2.0"},
                    {"method", "notifications/cancelled"},
                    {"params", {{"requestId", 7}, {"reason", "test"}}}})
                  .dump() +
              "\n");
    ASSERT_TRUE(wait_for([&] { return !m.fx.engine.generating.load(); }))
        << "cancellation did not stop the engine";
    // The server stays alive and serves further requests; the cancelled one is never answered.
    pipe.feed(modern_req(8, "server/discover").dump() + "\n");
    ASSERT_TRUE(wait_for([&] { return outbuf.lines().size() >= 2; }));
    pipe.close();
    srv.join();
    for (const auto& l : outbuf.lines()) {
        auto j = json::parse(l);
        EXPECT_NE(j["id"], 7) << "response sent for a cancelled request: " << l;
    }
    EXPECT_EQ(m.fx.cp->job_status(JobId{2}).value().state, JobState::Cancelled);
}

TEST(McpStdio, ConcurrentCallLimit) {
    McpOptions o;
    o.max_concurrent_calls = 1;
    Mcp m(o);
    m.fx.engine.run_until_cancel = true;
    m.fx.engine.delay_ms = 2;
    PipeBuf pipe;
    std::istream in(&pipe);
    OutBuf outbuf;
    std::ostream out(&outbuf);
    std::thread srv([&] { m.server.run(in, out); });
    pipe.feed(call_req(1, "hcs.models.load", {{"id", "tiny"}}).dump() + "\n");
    ASSERT_TRUE(wait_for([&] { return outbuf.lines().size() >= 1; }));
    pipe.feed(call_req(2, "hcs.chat.generate", {{"prompt", "go"}}).dump() + "\n");
    ASSERT_TRUE(wait_for([&] { return m.fx.engine.generating.load(); }));
    pipe.feed(call_req(3, "hcs.models.list").dump() + "\n");
    ASSERT_TRUE(wait_for([&] { return outbuf.lines().size() >= 2; }));
    auto busy = json::parse(outbuf.lines()[1]);
    EXPECT_EQ(busy["id"], 3);
    EXPECT_EQ(busy["result"]["structuredContent"]["error"]["code"], "busy");
    pipe.feed(
        json({{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", {{"requestId", 2}}}})
            .dump() +
        "\n");
    pipe.close();
    srv.join();
}

TEST(McpStdio, EofCancelsStragglersAfterGraceAndExits) {
    // Closing stdin while a call is running must not hang the process forever: in-flight work
    // finishes within the grace period or is cancelled. Here the call finishes by itself quickly.
    Mcp m;
    m.fx.engine.delay_ms = 10;
    std::stringstream in;
    in << call_req(1, "hcs.models.load", {{"id", "tiny"}}).dump() << "\n";
    std::ostringstream out;
    m.server.run(in, out);
    EXPECT_NE(out.str().find("\"id\":1"), std::string::npos);
}

// ---- M10 audit regressions
// ---------------------------------------------------------------------------------
namespace {

std::string nested(int depth) {
    return std::string(static_cast<std::size_t>(depth), '[') +
           std::string(static_cast<std::size_t>(depth), ']');
}

// One request line whose params carry a pathologically deep array next to a valid _meta.
std::string deep_params_line(const std::string& method, int depth, bool in_arguments) {
    std::string line = std::string("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"") + method +
                       "\",\"params\":{\"_meta\":" + meta().dump();
    if (in_arguments)
        line +=
            ",\"name\":\"hcs.models.inspect\",\"arguments\":{\"id\":\"tiny\",\"x\":" + nested(depth) + "}}}";
    else
        line += ",\"x\":" + nested(depth) + "}}";
    return line;
}

// FakeTextEngine is final: wrap it so a test can hook load().
class HookEngine final : public api::ITextStreamEngine {
public:
    test::FakeTextEngine fake;
    std::function<Result<void>(const LoadRequest&)> load_hook;
    Result<void> load(const LoadRequest& r) override { return load_hook ? load_hook(r) : fake.load(r); }
    Result<void> unload() override { return fake.unload(); }
    Result<JobId> generate(const TextRequest& r) override { return fake.generate(r); }
    Result<JobId> analyze_image(const VisionRequest& r) override { return fake.analyze_image(r); }
    EngineMetrics metrics() const override { return fake.metrics(); }
    Result<api::TextResult> generate_stream(const TextRequest& r, const api::TokenSink& sink,
                                            const CancellationToken& t) override {
        return fake.generate_stream(r, sink, t);
    }
};

// Control plane assembled by hand so a test can hook the engine.
struct CustomMcp {
    test::TempDir dir;
    ModelRegistry registry;
    PathGuard guard;
    MemoryBudget budget{0};
    std::ostringstream sink;
    Logger logger{&sink, LogLevel::Debug};
    JobQueue queue;
    HookEngine engine;
    std::unique_ptr<api::ControlPlane> cp;
    std::unique_ptr<McpServer> server;
    explicit CustomMcp(McpOptions o = {}) {
        const auto root = dir.path() / "models";
        test::write_file(root / "tiny.gguf", test::fake_gguf());
        (void)guard.add_root(root);
        (void)registry.scan(root, guard);
        cp = std::make_unique<api::ControlPlane>(registry, guard, budget, queue, &engine, logger);
        server = std::make_unique<McpServer>(*cp, o, logger);
    }
    ~CustomMcp() { queue.shutdown(); }
};

}  // namespace

// D1: a deeply nested document used to overflow the stack through a recursive deep copy of `params`.
TEST(McpAudit, DeeplyNestedParamsAreRejectedNotACrash) {
    Mcp m;
    for (int depth : {10000, 60000}) {
        for (bool in_args : {false, true}) {
            const std::string out =
                m.server.handle_line(deep_params_line(in_args ? "tools/call" : "tools/list", depth, in_args));
            ASSERT_FALSE(out.empty()) << "depth " << depth;
            auto j = json::parse(out);
            ASSERT_TRUE(j.contains("error")) << out.substr(0, 200);
            EXPECT_EQ(j["error"]["code"], -32600);
        }
    }
    auto ok = m.rpc(modern_req(2, "tools/list"));  // reasonable nesting is still served
    EXPECT_TRUE(ok.contains("result"));
}

TEST(McpAudit, DeeplyNestedLinesOverStdioAreRejectedAndServerKeepsServing) {
    Mcp m;
    std::stringstream in;
    in << deep_params_line("tools/list", 10000, false) << "\n"
       << deep_params_line("tools/call", 10000, true) << "\n"
       << "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"x\",\"params\":" << nested(200000) << "}\n"
       << modern_req(3, "tools/list").dump() << "\n";
    std::ostringstream out;
    m.server.run(in, out);
    std::istringstream lines(out.str());
    std::string l;
    int errors = 0;
    bool listed = false;
    while (std::getline(lines, l)) {
        auto j = json::parse(l);
        if (j.contains("error"))
            ++errors;
        if (j.contains("id") && j["id"] == 3 && j.contains("result"))
            listed = true;
    }
    EXPECT_EQ(errors, 3);
    EXPECT_TRUE(listed);
}

// S1: a second in-flight tools/call with the same id must not hijack the cancellation slot of the first.
TEST(McpAudit, DuplicateInFlightRequestIdIsRejected) {
    Mcp m;
    m.fx.engine.run_until_cancel = true;
    m.fx.engine.delay_ms = 2;
    PipeBuf pipe;
    std::istream in(&pipe);
    OutBuf outbuf;
    std::ostream out(&outbuf);
    std::thread srv([&] { m.server.run(in, out); });
    pipe.feed(call_req(1, "hcs.models.load", {{"id", "tiny"}}).dump() + "\n");
    ASSERT_TRUE(wait_for([&] { return outbuf.lines().size() >= 1; }));
    pipe.feed(call_req(5, "hcs.chat.generate", {{"prompt", "go"}, {"max_tokens", 100}}).dump() + "\n");
    ASSERT_TRUE(wait_for([&] { return m.fx.engine.generating.load(); }));
    pipe.feed(call_req(5, "hcs.models.list").dump() + "\n");
    ASSERT_TRUE(wait_for([&] { return outbuf.lines().size() >= 2; }));
    auto dup = json::parse(outbuf.lines()[1]);
    EXPECT_EQ(dup["id"], 5);
    ASSERT_TRUE(dup.contains("error")) << dup.dump();
    EXPECT_EQ(dup["error"]["code"], -32600);
    // The original call is still cancellable by its id.
    pipe.feed(
        json({{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", {{"requestId", 5}}}})
            .dump() +
        "\n");
    ASSERT_TRUE(wait_for([&] { return !m.fx.engine.generating.load(); }))
        << "cancel did not reach the first call";
    pipe.close();
    srv.join();
}

// C2: cancelling a running model load must release the MCP call promptly (its response is suppressed) instead
// of holding a call slot until the load finishes or the call timeout expires.
TEST(McpAudit, CancelDuringModelLoadReleasesTheCallSlotPromptly) {
    McpOptions o;
    o.max_concurrent_calls = 1;
    CustomMcp m(o);
    m.engine.load_hook = [&m](const LoadRequest& r) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        return m.engine.fake.load(r);
    };
    PipeBuf pipe;
    std::istream in(&pipe);
    OutBuf outbuf;
    std::ostream out(&outbuf);
    std::thread srv([&] { m.server->run(in, out); });
    pipe.feed(call_req(4, "hcs.models.load", {{"id", "tiny"}}).dump() + "\n");
    ASSERT_TRUE(wait_for([&] {
        auto st = m.cp->job_status(JobId{1});
        return st && st.value().state == JobState::Running;
    }));
    pipe.feed(
        json({{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", {{"requestId", 4}}}})
            .dump() +
        "\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(400));  // far below the 1.5 s the load still needs
    pipe.feed(call_req(6, "hcs.models.list").dump() + "\n");
    ASSERT_TRUE(wait_for([&] { return !outbuf.lines().empty(); }, 1000));
    auto second = json::parse(outbuf.lines().front());
    EXPECT_EQ(second["id"], 6);
    ASSERT_TRUE(second.contains("result")) << second.dump();
    EXPECT_FALSE(second["result"]["isError"])
        << "call slot was still held by the cancelled load: " << second.dump();
    pipe.close();
    srv.join();
    for (const auto& l : outbuf.lines())
        EXPECT_NE(json::parse(l)["id"], 4) << "response sent for a cancelled call";
}

// D6: engine/load errors reach MCP clients without absolute local paths.
TEST(McpAudit, ErrorsDoNotLeakAbsoluteLocalPaths) {
    CustomMcp m;
    m.engine.load_hook = [](const LoadRequest&) {
        return make_error(ErrorCode::ModelError,
                          "failed to load model from C:\\Users\\bob smith\\AppData\\Local\\Temp\\bad_f.gguf: "
                          "invalid magic");
    };
    auto r = json::parse(m.server->handle_line(call_req(1, "hcs.models.load", {{"id", "tiny"}}).dump()));
    const std::string text = r.dump();
    EXPECT_EQ(text.find("bob smith"), std::string::npos) << text;
    EXPECT_EQ(text.find("AppData"), std::string::npos) << text;
    EXPECT_EQ(text.find("Users"), std::string::npos) << text;
    EXPECT_NE(text.find("bad_f.gguf"), std::string::npos) << "the file name is kept for diagnosis: " << text;
    EXPECT_TRUE(r["result"]["isError"]);
}

// ---- stdio framing / JSON-RPC hardening (wave 3 test gaps) ---------------------------------------------
namespace {
std::vector<json> run_lines(Mcp& m, const std::string& input, std::size_t* raw_line_count = nullptr) {
    std::stringstream in(input);
    std::ostringstream out;
    m.server.run(in, out);
    std::vector<json> parsed;
    std::istringstream lines(out.str());
    std::string l;
    std::size_t n = 0;
    while (std::getline(lines, l)) {
        ++n;
        auto j = json::parse(l, nullptr, false);
        EXPECT_FALSE(j.is_discarded()) << "non-JSON line on stdout: " << l;
        parsed.push_back(std::move(j));
    }
    if (raw_line_count)
        *raw_line_count = n;
    return parsed;
}
}  // namespace

TEST(McpFraming, CrLfTerminatorsAndBlankLinesAreTolerated) {
    Mcp m;
    const std::string input = "\r\n   \r\n\n" + modern_req(1, "server/discover").dump() + "\r\n\r\n" +
                              modern_req(2, "tools/list").dump() + "\r\n";
    auto out = run_lines(m, input);
    ASSERT_EQ(out.size(), 2u) << "blank lines must produce no output";
    EXPECT_EQ(out[0]["id"], 1);
    EXPECT_EQ(out[1]["id"], 2);
}

TEST(McpFraming, FinalLineWithoutNewlineIsStillServed) {
    Mcp m;
    auto out = run_lines(m, modern_req(7, "server/discover").dump());  // no trailing '\n'
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0]["id"], 7);
}

TEST(McpFraming, InvalidUtf8AndEmbeddedNulAreParseErrorsAndTheStreamContinues) {
    Mcp m;
    std::string input;
    input += std::string("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"\xff\xfe\"}\n");
    input += std::string("{\"jsonrpc\":\"2.0\",\"id\":2,\"me\0thod\":\"x\"}\n", 38);
    input += std::string("\x00\x00\x00\n", 4);
    input += modern_req(3, "server/discover").dump() + "\n";
    auto out = run_lines(m, input);
    ASSERT_GE(out.size(), 2u);
    EXPECT_EQ(out.back()["id"], 3) << "the stream did not resynchronise after bad bytes";
    for (std::size_t i = 0; i + 1 < out.size(); ++i) {
        ASSERT_TRUE(out[i].contains("error")) << out[i].dump();
        EXPECT_TRUE(out[i]["error"]["code"] == -32700 || out[i]["error"]["code"] == -32600) << out[i].dump();
    }
}

TEST(McpFraming, LineAtTheLimitIsServedOneByteOverIsRejected) {
    McpOptions o;
    o.max_line_bytes = 512;
    Mcp m(o);
    json req = modern_req(1, "server/discover");
    req["params"]["pad"] = "";
    std::string base = req.dump();
    const std::size_t room = 512 - base.size();
    req["params"]["pad"] = std::string(room, 'p');
    const std::string exact = req.dump();
    ASSERT_EQ(exact.size(), 512u);
    req["id"] = 2;
    req["params"]["pad"] = std::string(room + 1, 'p');
    const std::string over = req.dump();
    ASSERT_GT(over.size(), 512u);
    auto out = run_lines(m, exact + "\n" + over + "\n" + modern_req(3, "server/discover").dump() + "\n");
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0]["id"], 1);
    EXPECT_TRUE(out[0].contains("result") || out[0].contains("error"));
    EXPECT_EQ(out[1]["error"]["code"], -32600) << "an oversize line must be rejected";
    EXPECT_EQ(out[2]["id"], 3);
}

TEST(McpFraming, ErrorStormKeepsStdoutOneJsonObjectPerLine) {
    Mcp m;
    std::string input;
    constexpr int kBad = 400;
    for (int i = 0; i < kBad; ++i) {
        switch (i % 5) {
            case 0:
                input += "not json at all\n";
                break;
            case 1:
                input += "{\"jsonrpc\":\"2.0\"\n";
                break;
            case 2:
                input += "[1,2,3]\n";
                break;
            case 3:
                input += "{\"jsonrpc\":\"1.0\",\"id\":1,\"method\":\"x\"}\n";
                break;
            default:
                input += "42\n";
                break;
        }
    }
    input += modern_req(99, "server/discover").dump() + "\n";
    std::size_t raw = 0;
    auto out = run_lines(m, input, &raw);
    EXPECT_EQ(raw, static_cast<std::size_t>(kBad + 1));
    ASSERT_EQ(out.size(), static_cast<std::size_t>(kBad + 1));
    for (int i = 0; i < kBad; ++i) {
        ASSERT_TRUE(out[static_cast<std::size_t>(i)].contains("error")) << i;
        EXPECT_EQ(out[static_cast<std::size_t>(i)]["jsonrpc"], "2.0");
    }
    EXPECT_EQ(out.back()["id"], 99);
}

TEST(McpFraming, ToolCallArgumentTypeConfusionIsAnErrorNeverACrash) {
    Mcp m;
    const json bad_params[] = {
        {{"name", 5}, {"arguments", json::object()}},
        {{"name", "hcs.models.list"}, {"arguments", "a string"}},
        {{"name", "hcs.models.list"}, {"arguments", json::array({1, 2})}},
        {{"name", "hcs.chat.generate"}, {"arguments", {{"prompt", json::array({1})}}}},
        {{"name", "hcs.chat.generate"}, {"arguments", {{"prompt", "hi"}, {"max_tokens", "many"}}}},
        {{"name", "hcs.models.load"}, {"arguments", {{"id", json::object()}}}},
        {{"name", "hcs.models.inspect"}, {"arguments", {{"id", nullptr}}}},
        {{"name", json::array({"hcs.models.list"})}, {"arguments", json::object()}},
        {{"arguments", json::object()}},
    };
    int id = 100;
    for (const auto& p : bad_params) {
        json params = p;
        params["_meta"] = meta();
        json req = {{"jsonrpc", "2.0"}, {"id", ++id}, {"method", "tools/call"}, {"params", params}};
        json r;
        ASSERT_NO_THROW(r = json::parse(m.server.handle_line(req.dump()))) << req.dump();
        const bool protocol_error = r.contains("error");
        const bool tool_error = r.contains("result") && r["result"].value("isError", false);
        EXPECT_TRUE(protocol_error || tool_error)
            << "accepted malformed call: " << req.dump() << " -> " << r.dump();
    }
    // Still healthy afterwards.
    EXPECT_TRUE(m.tool(500, "hcs.models.list").contains("result"));
}

TEST(McpFraming, UnusualRequestIdsDoNotBreakCorrelation) {
    Mcp m;
    const json ids[] = {std::string(10000, 'i'), "", 0, -1, 4294967296ll, 1.5, true};
    for (const auto& id : ids) {
        json req = modern_req(0, "server/discover");
        req["id"] = id;
        const std::string line = m.server.handle_line(req.dump());
        ASSERT_FALSE(line.empty());
        auto r = json::parse(line, nullptr, false);
        ASSERT_FALSE(r.is_discarded());
        if (r.contains("result"))
            EXPECT_EQ(r["id"], id) << "a valid id must be echoed unchanged";
        else
            EXPECT_TRUE(r.contains("error"));
    }
}

// --- overload protection (plan F1): the same structured errors and the kill switch over MCP ----------------
#include "fake_probe.hpp"

TEST(McpOverload, ShedToolWorksWithoutAControllerAndIsListedAsDestructive) {
    Mcp m;
    const auto r = m.rpc(modern_req(1, "tools/list"));
    bool found = false;
    for (const auto& t : r["result"]["tools"])
        if (t["name"] == "hcs.runtime.shed") {
            found = true;
            EXPECT_EQ(t["annotations"]["destructiveHint"], true);
        }
    EXPECT_TRUE(found);
    const auto res = m.tool(2, "hcs.runtime.shed");
    EXPECT_EQ(res["result"]["isError"], false);
    EXPECT_EQ(res["result"]["structuredContent"]["jobs_cancelled"], 0);
    EXPECT_EQ(res["result"]["structuredContent"]["model_unloaded"], false);
}

TEST(McpOverload, ChatIsRefusedWithTheSameStructuredErrorAsHttp) {
    test::FakeProbe probe;
    scheduler::AdmissionConfig cfg;
    auto adm = std::make_shared<scheduler::AdmissionController>(cfg, probe.probe());
    api::ControlPlaneOptions o;
    o.admission = adm;
    test::ApiFixture fx(true, o);
    McpServer server(*fx.cp, {}, fx.logger);
    auto call = [&](const std::string& tool, json args) {
        return json::parse(server.handle_line(call_req(1, tool, args).dump()));
    };
    probe.set_free(2000 * test::kMiB);  // below the soft floor
    const auto r = call("hcs.chat.generate", {{"prompt", "hi"}, {"model", "tiny"}});
    EXPECT_EQ(r["result"]["isError"], true);
    EXPECT_EQ(r["result"]["structuredContent"]["error"]["code"], "overloaded");
    // The kill switch tool records itself on the controller.
    const auto s = call("hcs.runtime.shed", json::object());
    EXPECT_EQ(s["result"]["isError"], false);
    EXPECT_EQ(adm->snapshot_json()["counters"]["shed_count"], 1);
}
