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
    explicit Mcp(McpOptions o = {}, bool with_engine = true) : fx(with_engine), server(*fx.cp, o, fx.logger) {}
    json rpc(const json& req) {
        const std::string line = server.handle_line(req.dump());
        return line.empty() ? json(nullptr) : json::parse(line);
    }
    json tool(int id, const std::string& name, json args = json::object()) { return rpc(call_req(id, name, args)); }
};

bool wait_for(const std::function<bool()>& f, int ms = 3000) {
    for (int i = 0; i < ms / 5; ++i) {
        if (f()) return true;
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
        if (pos_ >= data_.size()) return traits_type::eof();
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
    auto init = m.rpc({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "initialize"},
                       {"params", {{"protocolVersion", kLegacyVersion}, {"capabilities", json::object()}, {"clientInfo", {{"name", "old"}, {"version", "1"}}}}}});
    ASSERT_TRUE(init.contains("result")) << init.dump();
    EXPECT_EQ(init["result"]["protocolVersion"], kLegacyVersion);
    EXPECT_TRUE(init["result"]["capabilities"].contains("tools"));
    EXPECT_FALSE(init["result"].contains("resultType"));
    // Not usable until notifications/initialized
    EXPECT_TRUE(m.rpc({{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/list"}}).contains("error"));
    EXPECT_TRUE(m.rpc({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}).is_null());  // notification: no reply
    auto ping = m.rpc({{"jsonrpc", "2.0"}, {"id", 4}, {"method", "ping"}});
    EXPECT_TRUE(ping["result"].is_object());
    auto list = m.rpc({{"jsonrpc", "2.0"}, {"id", 5}, {"method", "tools/list"}});
    ASSERT_TRUE(list.contains("result"));
    EXPECT_GE(list["result"]["tools"].size(), 11u);
    auto call = m.rpc({{"jsonrpc", "2.0"}, {"id", 6}, {"method", "tools/call"}, {"params", {{"name", "hcs.models.list"}, {"arguments", json::object()}}}});
    EXPECT_FALSE(call["result"]["isError"].get<bool>());
}

TEST(McpProtocol, MalformedMessages) {
    Mcp m;
    auto parse = json::parse(m.server.handle_line("{not json"));
    EXPECT_EQ(parse["error"]["code"], -32700);
    EXPECT_TRUE(parse["id"].is_null());
    EXPECT_EQ(json::parse(m.server.handle_line("[]"))["error"]["code"], -32600);          // batches unsupported
    EXPECT_EQ(json::parse(m.server.handle_line(R"({"id":1,"method":"x"})"))["error"]["code"], -32600);  // no jsonrpc
    EXPECT_EQ(json::parse(m.server.handle_line(R"({"jsonrpc":"2.0","id":{"a":1},"method":"x"})"))["error"]["code"], -32600);
    EXPECT_EQ(json::parse(m.server.handle_line(R"({"jsonrpc":"2.0","id":1,"method":5})"))["error"]["code"], -32600);
    EXPECT_EQ(json::parse(m.server.handle_line(R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":[]})"))["error"]["code"], -32602);
    EXPECT_TRUE(m.server.handle_line(R"({"jsonrpc":"2.0","method":"notifications/unknown"})").empty());
    EXPECT_TRUE(m.server.handle_line(R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":999}})").empty());
}

// ---- tools --------------------------------------------------------------------------------------------------
TEST(McpTools, ListHasNarrowSchemasForEveryRequiredTool) {
    Mcp m;
    auto r = m.rpc(modern_req(1, "tools/list"));
    std::set<std::string> names;
    for (const auto& t : r["result"]["tools"]) {
        const std::string n = t["name"];
        names.insert(n);
        EXPECT_LE(n.size(), 128u);
        for (char c : n) EXPECT_TRUE(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.') << n;
        EXPECT_EQ(t["inputSchema"]["type"], "object") << n;
        EXPECT_EQ(t["inputSchema"]["additionalProperties"], false) << "schema for " << n << " must be closed";
        EXPECT_TRUE(t.contains("description"));
        EXPECT_TRUE(t["annotations"].contains("readOnlyHint"));
    }
    for (const char* want : {"hcs.models.list", "hcs.models.inspect", "hcs.models.load", "hcs.models.unload", "hcs.chat.generate",
                             "hcs.runtime.status", "hcs.jobs.status", "hcs.jobs.cancel", "hcs.vision.analyze",
                             "hcs.image.generate", "hcs.image.edit"})
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
    for (const json& args : {json{{"id", 5}}, json{{"id", "tiny"}, {"extra", 1}}, json::object(), json{{"id", std::string(100, 'a')}}}) {
        auto r = m.tool(1, "hcs.models.inspect", args);
        ASSERT_TRUE(r.contains("result")) << args.dump() << " -> " << r.dump();
        EXPECT_TRUE(r["result"]["isError"].get<bool>()) << args.dump();
        EXPECT_EQ(r["result"]["structuredContent"]["error"]["code"], "invalid_argument");
    }
    auto unknown = m.tool(2, "hcs.rm.rf");
    EXPECT_EQ(unknown["error"]["code"], -32602);
    EXPECT_EQ(m.rpc(modern_req(3, "tools/call", {{"name", 5}}))["error"]["code"], -32602);
    EXPECT_EQ(m.rpc(modern_req(4, "tools/call", {{"name", "hcs.models.list"}, {"arguments", "x"}}))["error"]["code"], -32602);
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
    auto chat2 = m.tool(7, "hcs.chat.generate", {{"model", "other"}, {"messages", json::array({json{{"role", "user"}, {"content", "x"}}})}});
    EXPECT_FALSE(chat2["result"]["isError"].get<bool>()) << chat2.dump();
}

TEST(McpTools, ChatNeedsExactlyOneOfPromptOrMessages) {
    Mcp m;
    EXPECT_TRUE(m.tool(1, "hcs.chat.generate", {{"model", "tiny"}})["result"]["isError"].get<bool>());
    EXPECT_TRUE(m.tool(2, "hcs.chat.generate", {{"model", "tiny"}, {"prompt", "a"}, {"messages", json::array({json{{"role", "user"}, {"content", "b"}}})}})["result"]["isError"].get<bool>());
    EXPECT_TRUE(m.tool(3, "hcs.chat.generate", {{"model", "tiny"}, {"prompt", "a"}, {"max_tokens", 100000000}})["result"]["isError"].get<bool>());
}

TEST(McpTools, ModelIdPathTraversalIsRejected) {
    Mcp m;
    for (const char* id : {"../../etc/passwd", "..\\..\\windows\\system32", "C:\\windows\\win.ini", "tiny/../other"}) {
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
    auto esc = m.tool(1, "hcs.vision.analyze", {{"prompt", "what"}, {"image_path", (outside / "secret.png").string()}});
    EXPECT_TRUE(esc["result"]["isError"].get<bool>());
    EXPECT_EQ(esc["result"]["structuredContent"]["error"]["code"], "security_violation");
    auto dots = m.tool(2, "hcs.vision.analyze", {{"prompt", "what"}, {"image_path", (m.fx.root / ".." / "x.png").string()}});
    EXPECT_EQ(dots["result"]["structuredContent"]["error"]["code"], "security_violation");
    {
        // a real (generated) PNG: the fake engine has no vision, so the call reaches the engine and is "unsupported"
        const auto png = test::make_red_square_png(32);
        test::write_file(m.fx.root / "ok.png", std::string(png.begin(), png.end()));
    }
    auto ok = m.tool(3, "hcs.vision.analyze", {{"prompt", "what"}, {"image_path", (m.fx.root / "ok.png").string()}});
    EXPECT_EQ(ok["result"]["structuredContent"]["error"]["code"], "unsupported");
    EXPECT_EQ(m.tool(4, "hcs.image.generate", {{"prompt", "a cat"}})["result"]["structuredContent"]["error"]["code"], "unsupported");
    EXPECT_EQ(m.tool(5, "hcs.image.edit", {{"prompt", "a cat"}, {"input_path", (outside / "secret.png").string()}})["result"]["structuredContent"]["error"]["code"], "security_violation");
}

TEST(McpTools, JobIdsAreValidated) {
    Mcp m;
    EXPECT_EQ(m.tool(1, "hcs.jobs.status", {{"job_id", "banana"}})["result"]["structuredContent"]["error"]["code"], "invalid_argument");
    EXPECT_EQ(m.tool(2, "hcs.jobs.status", {{"job_id", "job-777"}})["result"]["structuredContent"]["error"]["code"], "not_found");
    EXPECT_EQ(m.tool(3, "hcs.jobs.cancel", {{"job_id", "job-777"}})["result"]["structuredContent"]["error"]["code"], "not_found");
}

TEST(McpTools, NoEngineIsReportedAsUnsupported) {
    Mcp m({}, false);
    EXPECT_EQ(m.tool(1, "hcs.models.load", {{"id", "tiny"}})["result"]["structuredContent"]["error"]["code"], "unsupported");
    EXPECT_EQ(m.tool(2, "hcs.chat.generate", {{"prompt", "x"}})["result"]["structuredContent"]["error"]["code"], "unsupported");
}

// ---- stdio loop ----------------------------------------------------------------------------------------------
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
        if (j["id"].is_number()) answered.insert(j["id"].get<int>());
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
    pipe.feed(json({{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", {{"requestId", 7}, {"reason", "test"}}}}).dump() + "\n");
    ASSERT_TRUE(wait_for([&] { return !m.fx.engine.generating.load(); })) << "cancellation did not stop the engine";
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
    pipe.feed(json({{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", {{"requestId", 2}}}}).dump() + "\n");
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
