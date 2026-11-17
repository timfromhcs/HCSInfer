// Contract tests for image input through ControlPlane, the HTTP chat route and the MCP vision tool.
// A deterministic FakeVisionEngine exercises plumbing only (limits, validation, projector pairing,
// streaming, error mapping). None of this is evidence of inference: the real model test lives in
// tests/integration/llama_vision (AGENTS.md 16.2).
#include <gtest/gtest.h>

#include <atomic>
#include <mutex>

#include "hcs/api/control_plane.hpp"
#include "hcs/api/http_api.hpp"
#include "hcs/mcp/mcp_server.hpp"
#include "temp_dir.hpp"
#include "test_png.hpp"

using namespace hcs;
using namespace hcs::api;
using nlohmann::json;

namespace {

const char* kToken = "test-token-0123456789abcdef";

class FakeVisionEngine final : public ITextStreamEngine, public IVisionStreamEngine {
public:
    std::mutex mu;
    LoadRequest last_load;
    VisionRequest last_vision;
    std::atomic<int> vision_calls{0}, text_calls{0};
    bool projector_loaded = false;

    Result<void> load(const LoadRequest& r) override {
        std::lock_guard lk(mu);
        last_load = r;
        projector_loaded = !r.projector_path.empty();
        return {};
    }
    Result<void> unload() override {
        projector_loaded = false;
        return {};
    }
    Result<JobId> generate(const TextRequest&) override { return make_error(ErrorCode::Unsupported, "fake"); }
    Result<JobId> analyze_image(const VisionRequest&) override { return make_error(ErrorCode::Unsupported, "fake"); }
    EngineMetrics metrics() const override { return {}; }
    bool vision_loaded() const override { return projector_loaded; }

    Result<TextResult> generate_stream(const TextRequest&, const TokenSink& sink, const CancellationToken&) override {
        ++text_calls;
        sink("text only");
        return TextResult{};
    }
    Result<TextResult> analyze_image_stream(const VisionRequest& r, const TokenSink& sink,
                                            const CancellationToken&) override {
        {
            std::lock_guard lk(mu);
            last_vision = r;
        }
        ++vision_calls;
        if (!projector_loaded) return make_error(ErrorCode::Unsupported, "fake: no projector");
        for (const char* p : {"A red", " square", " on white."})
            if (!sink(p)) break;
        TextResult t;
        t.completion_tokens = 3;
        return t;
    }
};

struct Fixture {
    test::TempDir dir;
    std::filesystem::path root;
    std::ostringstream log_sink;
    Logger logger{&log_sink, LogLevel::Debug};
    ModelRegistry registry;
    PathGuard guard;
    MemoryBudget budget{0};
    std::unique_ptr<JobQueue> queue;
    FakeVisionEngine engine;
    std::unique_ptr<ControlPlane> cp;
    std::string vlm_id, plain_id;

    explicit Fixture(ControlPlaneOptions opts = {}) {
        root = dir.path() / "models";
        test::write_file(root / "vlm" / "smol-Q8_0.gguf", test::fake_gguf());
        test::write_file(root / "vlm" / "mmproj-smol-f16.gguf", test::fake_gguf());
        test::write_file(root / "plain" / "plain.gguf", test::fake_gguf());
        (void)guard.add_root(root);
        (void)registry.scan(root, guard);
        for (const auto& m : registry.list()) {
            if (m.path.find("smol-Q8_0") != std::string::npos) vlm_id = m.id;
            if (m.path.find("plain.gguf") != std::string::npos) plain_id = m.id;
        }
        queue = std::make_unique<JobQueue>();
        opts.version = "test";
        cp = std::make_unique<ControlPlane>(registry, guard, budget, *queue, &engine, logger, opts);
    }
    ~Fixture() { queue->shutdown(); }

    ChatParams vision_params(const std::string& model, std::vector<std::uint8_t> png = test::make_red_square_png(32)) {
        ChatParams p;
        p.model = model;
        ChatMessage m;
        m.role = "user";
        m.content = "what colour is the square?";
        m.images.push_back(ImageInput{std::move(png)});
        p.messages.push_back(std::move(m));
        return p;
    }

    // Runs a submitted chat job to completion; returns {state, text, error code}.
    struct Out {
        JobState state;
        std::string text;
        std::string code;
    };
    Out run(const ChatParams& p) {
        auto sub = cp->submit_chat(p);
        if (!sub) return {JobState::Failed, "", std::string(to_string(sub.error().code))};
        Out o{JobState::Queued, "", ""};
        while (true) {
            JobEvent ev;
            const auto r = sub.value().channel->pop(ev, std::chrono::steady_clock::now() + std::chrono::seconds(10));
            if (r == JobChannel::Pop::Event) {
                if (ev.kind == JobEventKind::Output) o.text += ev.text;
            } else {
                break;
            }
        }
        (void)cp->wait_job(sub.value().id, std::chrono::seconds(5));
        auto st = cp->job_status(sub.value().id);
        o.state = st.value().state;
        if (st.value().error) o.code = std::string(to_string(st.value().error->code));
        return o;
    }
};

}  // namespace

// ---- projector pairing ------------------------------------------------------------------------
TEST(VisionPairing, NameMatchPairsMmprojIgnoringQuantisation) {
    Fixture f;
    auto m = f.registry.get(f.vlm_id);
    ASSERT_TRUE(m.ok());
    auto p = f.cp->projector_path_for(m.value());
    ASSERT_TRUE(p.ok()) << p.error().message;
    EXPECT_NE(p.value().find("mmproj-smol-f16.gguf"), std::string::npos);

    auto plain = f.registry.get(f.plain_id);
    ASSERT_TRUE(plain.ok());
    auto none = f.cp->projector_path_for(plain.value());
    ASSERT_TRUE(none.ok());
    EXPECT_TRUE(none.value().empty());  // different folder: never paired
}

TEST(VisionPairing, AmbiguousFolderIsNotGuessed) {
    test::TempDir dir;
    const auto root = dir.path() / "m";
    test::write_file(root / "alpha.gguf", test::fake_gguf());
    test::write_file(root / "beta.gguf", test::fake_gguf());
    test::write_file(root / "mmproj-gamma.gguf", test::fake_gguf());  // belongs to neither
    ModelRegistry reg;
    PathGuard guard;
    (void)guard.add_root(root);
    (void)reg.scan(root, guard);
    MemoryBudget budget{0};
    JobQueue q;
    FakeVisionEngine e;
    std::ostringstream sink;
    Logger lg{&sink, LogLevel::Debug};
    ControlPlane cp(reg, guard, budget, q, &e, lg);
    for (const auto& m : reg.list()) {
        auto p = cp.projector_path_for(m);
        ASSERT_TRUE(p.ok());
        EXPECT_TRUE(p.value().empty()) << m.id;  // a projector is never loaded as a model's partner by guesswork
    }
    q.shutdown();
}

// ---- control plane -----------------------------------------------------------------------------
TEST(VisionControlPlane, LoadsProjectorAndStreamsTheAnswer) {
    Fixture f;
    auto o = f.run(f.vision_params(f.vlm_id));
    EXPECT_EQ(o.state, JobState::Succeeded) << o.code;
    EXPECT_EQ(o.text, "A red square on white.");
    {
        std::lock_guard lk(f.engine.mu);
        EXPECT_NE(f.engine.last_load.projector_path.find("mmproj-smol-f16.gguf"), std::string::npos);
        EXPECT_EQ(f.engine.last_vision.text.prompt, "what colour is the square?");
        EXPECT_EQ(f.engine.last_vision.image.bytes, test::make_red_square_png(32));
        EXPECT_TRUE(f.engine.last_vision.text.apply_chat_template);
    }
    EXPECT_TRUE(f.cp->loaded_has_vision());
    EXPECT_EQ(f.engine.text_calls.load(), 0);
}

TEST(VisionControlPlane, ModelWithoutProjectorIsACleanUnsupportedError) {
    Fixture f;
    auto o = f.run(f.vision_params(f.plain_id));
    EXPECT_EQ(o.state, JobState::Failed);
    EXPECT_EQ(o.code, "unsupported");
    EXPECT_EQ(f.engine.vision_calls.load(), 0);  // never reaches the engine
    EXPECT_FALSE(f.cp->loaded_has_vision());
}

TEST(VisionControlPlane, PlainTextChatStillUsesTheTextPath) {
    Fixture f;
    ChatParams p;
    p.model = f.vlm_id;
    p.messages.push_back({"user", "hello", {}});
    auto o = f.run(p);
    EXPECT_EQ(o.state, JobState::Succeeded);
    EXPECT_EQ(o.text, "text only");
    EXPECT_EQ(f.engine.text_calls.load(), 1);
    EXPECT_EQ(f.engine.vision_calls.load(), 0);
}

TEST(VisionControlPlane, ValidationRejectsBadShapesAndImages) {
    ControlPlaneOptions opts;
    opts.max_image_bytes = 100000;
    opts.max_image_pixels = 1000;  // 31x31
    Fixture f(opts);

    auto good = f.vision_params(f.vlm_id, test::make_red_square_png(16));
    EXPECT_TRUE(f.cp->validate_chat(good).ok());

    auto two = good;  // two images
    two.messages[0].images.push_back(two.messages[0].images[0]);
    EXPECT_FALSE(f.cp->validate_chat(two).ok());

    auto asst = good;  // image on an assistant message
    asst.messages[0].role = "assistant";
    EXPECT_FALSE(f.cp->validate_chat(asst).ok());

    auto multi = good;  // multi-turn with an image
    multi.messages.insert(multi.messages.begin(), ChatMessage{"user", "earlier", {}});
    multi.messages.insert(multi.messages.begin() + 1, ChatMessage{"assistant", "ok", {}});
    EXPECT_FALSE(f.cp->validate_chat(multi).ok());

    auto notext = good;
    notext.messages[0].content.clear();
    EXPECT_FALSE(f.cp->validate_chat(notext).ok());

    auto garbage = good;
    garbage.messages[0].images[0].bytes = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25};
    auto g = f.cp->validate_chat(garbage);
    ASSERT_FALSE(g.ok());
    EXPECT_EQ(g.error().code, ErrorCode::InvalidArgument);

    auto huge_dims = good;  // header declares 40000x40000 but carries no pixels
    huge_dims.messages[0].images[0].bytes = test::make_png_header_only(40000, 40000);
    auto h = f.cp->validate_chat(huge_dims);
    ASSERT_FALSE(h.ok());
    EXPECT_NE(h.error().message.find("pixel limit"), std::string::npos);

    auto big = good;  // above the byte cap
    big.messages[0].images[0].bytes.assign(100001, 0);
    EXPECT_FALSE(f.cp->validate_chat(big).ok());
    EXPECT_EQ(f.engine.vision_calls.load(), 0);
}

// ---- HTTP -------------------------------------------------------------------------------------
namespace {

struct Http {
    Fixture fx;
    ApiHandler handler;
    explicit Http(ControlPlaneOptions cpo = {}) : fx(cpo), handler(*fx.cp, opts(), fx.logger) {}
    static ApiOptions opts() {
        ApiOptions o;
        o.token = kToken;
        o.port = 8765;
        o.max_body_bytes = 4u << 20;
        return o;
    }
    HttpResponse post(const json& body) {
        HttpRequest r;
        r.method = "POST";
        r.path = "/v1/chat/completions";
        r.body = body.dump();
        r.headers["host"] = "127.0.0.1:8765";
        r.headers["authorization"] = std::string("Bearer ") + kToken;
        r.headers["content-type"] = "application/json";
        return handler.handle(r);
    }
    json vision_body(const std::string& model, const std::string& uri, bool stream = false) {
        return {{"model", model},
                {"stream", stream},
                {"max_tokens", 32},
                {"messages",
                 json::array({json{{"role", "user"},
                                   {"content", json::array({json{{"type", "text"}, {"text", "what colour is the square?"}},
                                                            json{{"type", "image_url"}, {"image_url", {{"url", uri}}}}})}}})}};
    }
};

std::string collect(HttpResponse& r) {
    std::string all;
    r.stream([&](std::string_view c) {
        all.append(c);
        return true;
    });
    return all;
}

}  // namespace

TEST(VisionHttp, DataUriImageReturnsTheModelAnswer) {
    Http h;
    auto r = h.post(h.vision_body(h.fx.vlm_id, test::png_data_uri(test::make_red_square_png(32))));
    ASSERT_EQ(r.status, 200) << r.body;
    auto j = json::parse(r.body);
    EXPECT_EQ(j["choices"][0]["message"]["content"], "A red square on white.");
    EXPECT_EQ(h.fx.engine.vision_calls.load(), 1);
    EXPECT_EQ(h.fx.engine.last_vision.image.bytes, test::make_red_square_png(32));
}

TEST(VisionHttp, StreamingWorksWithImages) {
    Http h;
    auto r = h.post(h.vision_body(h.fx.vlm_id, test::png_data_uri(test::make_red_square_png(32)), true));
    ASSERT_EQ(r.status, 200);
    ASSERT_TRUE(static_cast<bool>(r.stream));
    const std::string all = collect(r);
    EXPECT_NE(all.find("A red"), std::string::npos);
    EXPECT_NE(all.find("[DONE]"), std::string::npos);
}

TEST(VisionHttp, RemoteUrlsPathsAndBadDataUrisAreRejected) {
    Http h;
    for (const char* u : {"http://example.com/a.png", "https://example.com/a.png", "file:///C:/Windows/win.ini",
                          "C:\\Windows\\win.ini", "data:image/png;base64,@@@@", "data:text/html;base64,QUJD"}) {
        auto r = h.post(h.vision_body(h.fx.vlm_id, u));
        EXPECT_EQ(r.status, 400) << u << " -> " << r.body;
    }
    EXPECT_EQ(h.fx.engine.vision_calls.load(), 0);
}

TEST(VisionHttp, NonImageBytesAndOversizedDimensionsAre400) {
    ControlPlaneOptions cpo;
    cpo.max_image_pixels = 4096;
    Http h(cpo);
    auto not_img = h.post(h.vision_body(h.fx.vlm_id, "data:image/png;base64,QUJDREVGR0hJSktMTU5PUFFSU1RVVldY"));
    EXPECT_EQ(not_img.status, 400) << not_img.body;
    auto big = h.post(h.vision_body(h.fx.vlm_id, test::png_data_uri(test::make_png_header_only(100, 100))));
    EXPECT_EQ(big.status, 400) << big.body;
    EXPECT_EQ(h.fx.engine.vision_calls.load(), 0);
}

TEST(VisionHttp, ModelWithoutProjectorIs501NotAFakeAnswer) {
    Http h;
    auto r = h.post(h.vision_body(h.fx.plain_id, test::png_data_uri(test::make_red_square_png(32))));
    EXPECT_EQ(r.status, 501) << r.body;
    EXPECT_NE(r.body.find("vision projector"), std::string::npos);
}

TEST(VisionHttp, ImageInSystemMessageIsRejected) {
    Http h;
    json b = h.vision_body(h.fx.vlm_id, test::png_data_uri(test::make_red_square_png(32)));
    b["messages"][0]["role"] = "system";
    EXPECT_EQ(h.post(b).status, 400);
}

// ---- MCP --------------------------------------------------------------------------------------
namespace {

json call_req(int id, const std::string& tool, json args) {
    json meta = {{"io.modelcontextprotocol/protocolVersion", mcp::kModernVersion},
                 {"io.modelcontextprotocol/clientInfo", {{"name", "test"}, {"version", "0"}}},
                 {"io.modelcontextprotocol/clientCapabilities", json::object()}};
    return {{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"},
            {"params", {{"name", tool}, {"arguments", args}, {"_meta", meta}}}};
}

}  // namespace

TEST(VisionMcp, AnalyzeReadsApprovedPathAndReturnsText) {
    Fixture f;
    mcp::McpServer server(*f.cp, {}, f.logger);
    const auto png = test::make_red_square_png(32);
    test::write_file(f.root / "img.png", std::string(png.begin(), png.end()));
    auto r = json::parse(server.handle_line(call_req(1, "hcs.vision.analyze",
                                                     {{"model", f.vlm_id},
                                                      {"prompt", "colour?"},
                                                      {"image_path", (f.root / "img.png").string()}})
                                                .dump()));
    ASSERT_FALSE(r["result"]["isError"].get<bool>()) << r.dump();
    EXPECT_EQ(r["result"]["structuredContent"]["text"], "A red square on white.");
    EXPECT_EQ(f.engine.last_vision.image.bytes, png);
}

TEST(VisionMcp, PathEscapesNonImagesAndMissingProjectorAreCleanErrors) {
    Fixture f;
    mcp::McpServer server(*f.cp, {}, f.logger);
    test::TempDir outside;
    const auto png = test::make_red_square_png(16);
    test::write_file(outside / "secret.png", std::string(png.begin(), png.end()));
    auto code_of = [&](int id, json args) {
        auto r = json::parse(server.handle_line(call_req(id, "hcs.vision.analyze", args).dump()));
        EXPECT_TRUE(r["result"]["isError"].get<bool>()) << r.dump();
        return r["result"]["structuredContent"]["error"]["code"].get<std::string>();
    };
    EXPECT_EQ(code_of(1, {{"prompt", "x"}, {"image_path", (outside / "secret.png").string()}}), "security_violation");
    test::write_file(f.root / "notimg.png", "this is not an image at all, just text bytes");
    EXPECT_EQ(code_of(2, {{"model", f.vlm_id}, {"prompt", "x"}, {"image_path", (f.root / "notimg.png").string()}}), "invalid_argument");
    test::write_file(f.root / "ok.png", std::string(png.begin(), png.end()));
    EXPECT_EQ(code_of(3, {{"model", f.plain_id}, {"prompt", "x"}, {"image_path", (f.root / "ok.png").string()}}), "unsupported");
    EXPECT_EQ(f.engine.vision_calls.load(), 0);
}
