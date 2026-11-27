// Contract tests for the MCP tools hcs.image.generate / hcs.image.edit. FakeImageEngine = plumbing only, not
// evidence of image generation (AGENTS.md 16.2); real output is covered by tests/e2e/image_real.
#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "hcs/mcp/mcp_server.hpp"
#include "image_api_fixture.hpp"

using namespace hcs;
using namespace hcs::mcp;
using nlohmann::json;

namespace {

json meta() {
    return {{"io.modelcontextprotocol/protocolVersion", kModernVersion},
            {"io.modelcontextprotocol/clientInfo", {{"name", "test"}, {"version", "0"}}},
            {"io.modelcontextprotocol/clientCapabilities", json::object()}};
}
json call_req(int id, const std::string& tool, json args = json::object()) {
    json params = {{"name", tool}, {"arguments", args}, {"_meta", meta()}};
    return {{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"}, {"params", params}};
}

struct Mcp {
    test::ImageApiFixture fx;
    McpServer server;
    explicit Mcp(McpOptions o = {}, bool with_image = true) : fx(with_image), server(*fx.cp, o, fx.logger) {}
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

std::string code_of(const json& r) {
    return r["result"]["structuredContent"]["error"]["code"];
}

}  // namespace

TEST(McpImage, ToolsAreListedWithRealSchemas) {
    json tools = tool_list(256);
    bool gen = false, edit = false;
    for (const auto& t : tools) {
        const std::string n = t["name"];
        if (n == "hcs.image.generate") {
            gen = true;
            EXPECT_EQ(t["description"].get<std::string>().find("Currently reports"), std::string::npos);
            EXPECT_TRUE(t["inputSchema"]["properties"].contains("cfg_scale"));
        }
        if (n == "hcs.image.edit") {
            edit = true;
            EXPECT_TRUE(t["inputSchema"]["properties"].contains("strength"));
        }
    }
    EXPECT_TRUE(gen);
    EXPECT_TRUE(edit);
}

TEST(McpImage, GenerateReturnsAnImageContentBlock) {
    Mcp m;
    auto r = m.tool(1, "hcs.image.generate",
                    {{"model", "sdmodel"},
                     {"prompt", "a red apple"},
                     {"width", 256},
                     {"height", 256},
                     {"steps", 2},
                     {"seed", 3}});
    ASSERT_FALSE(r["result"]["isError"].get<bool>()) << r.dump();
    const auto& content = r["result"]["content"];
    ASSERT_EQ(content[0]["type"], "image");
    EXPECT_EQ(content[0]["mimeType"], "image/png");
    EXPECT_EQ(content[0]["data"].get<std::string>().rfind("iVBORw0KGgo", 0), 0u);
    const auto& sc = r["result"]["structuredContent"];
    EXPECT_EQ(sc["width"], 256);
    EXPECT_EQ(sc["height"], 256);
    EXPECT_EQ(sc["steps"], 2);
    EXPECT_FALSE(sc.contains("data")) << "pixels are not duplicated into structuredContent";
    EXPECT_EQ(m.fx.image.generations.load(), 1);
    EXPECT_EQ(m.fx.image.last.width, 256u);
    ASSERT_TRUE(m.fx.image.last.seed.has_value());
    EXPECT_EQ(*m.fx.image.last.seed, 3u);
}

TEST(McpImage, GenerateValidatesEverythingBeforeTheEngine) {
    Mcp m;
    EXPECT_EQ(
        code_of(m.tool(1, "hcs.image.generate", {{"model", "sdmodel"}, {"prompt", "x"}, {"width", 250}})),
        "invalid_argument");
    EXPECT_EQ(code_of(m.tool(2, "hcs.image.generate",
                             {{"model", "sdmodel"}, {"prompt", "x"}, {"width", 2048}, {"height", 2048}})),
              "invalid_argument");
    EXPECT_EQ(
        code_of(m.tool(3, "hcs.image.generate", {{"model", "sdmodel"}, {"prompt", "x"}, {"steps", 1000}})),
        "invalid_argument");
    EXPECT_EQ(code_of(m.tool(4, "hcs.image.generate", {{"model", "tiny"}, {"prompt", "x"}})),
              "invalid_argument")
        << "text model";
    EXPECT_EQ(code_of(m.tool(5, "hcs.image.generate", {{"model", "nope"}, {"prompt", "x"}})), "not_found");
    EXPECT_EQ(code_of(m.tool(6, "hcs.image.generate", {{"model", "sdmodel"}, {"prompt", "x"}, {"extra", 1}})),
              "invalid_argument");
    EXPECT_EQ(code_of(m.tool(7, "hcs.image.generate",
                             {{"prompt", "x"}, {"width", 64}, {"height", 64}, {"steps", 1}})),
              "invalid_state")
        << "no image model loaded";
    EXPECT_EQ(m.fx.image.generations.load(), 0);
}

TEST(McpImage, NoImageEngineIsUnsupportedNotFake) {
    Mcp m({}, /*with_image=*/false);
    EXPECT_EQ(code_of(m.tool(1, "hcs.image.generate", {{"prompt", "a cat"}})), "unsupported");
}

TEST(McpImage, EditReadsOnlyFromApprovedRootsAndKeepsTheInputSize) {
    Mcp m;
    test::TempDir outside;
    const auto png = test::make_red_square_png(64);
    test::write_file(outside / "secret.png", std::string(png.begin(), png.end()));
    // outside the approved root: refused before anything is read, even though the file is a valid PNG
    EXPECT_EQ(
        code_of(m.tool(
            1, "hcs.image.edit",
            {{"model", "sdmodel"}, {"prompt", "x"}, {"input_path", (outside / "secret.png").string()}})),
        "security_violation");
    EXPECT_EQ(code_of(m.tool(2, "hcs.image.edit",
                             {{"prompt", "x"}, {"input_path", (m.fx.root / ".." / "x.png").string()}})),
              "security_violation");
    EXPECT_EQ(m.fx.image.edits.load(), 0);

    test::write_file(m.fx.root / "in.png", std::string(png.begin(), png.end()));
    auto r = m.tool(3, "hcs.image.edit",
                    {{"model", "sdmodel"},
                     {"prompt", "make it blue"},
                     {"input_path", (m.fx.root / "in.png").string()},
                     {"steps", 2},
                     {"strength", 0.4}});
    ASSERT_FALSE(r["result"]["isError"].get<bool>()) << r.dump();
    EXPECT_EQ(r["result"]["content"][0]["type"], "image");
    EXPECT_EQ(m.fx.image.edits.load(), 1);
    EXPECT_EQ(m.fx.image.last.width, 64u);
    ASSERT_TRUE(m.fx.image.last.init_image.has_value());
    EXPECT_EQ(m.fx.image.last.init_image->bytes, png);

    // a non-image file inside the root is a clean error, not a decoder run
    test::write_file(m.fx.root / "junk.png", "not an image at all");
    EXPECT_EQ(code_of(m.tool(4, "hcs.image.edit",
                             {{"model", "sdmodel"},
                              {"prompt", "x"},
                              {"input_path", (m.fx.root / "junk.png").string()},
                              {"width", 64},
                              {"height", 64}})),
              "invalid_argument");
    EXPECT_EQ(m.fx.image.edits.load(), 1);
}

TEST(McpImage, CancellationNotificationStopsTheRender) {
    Mcp m;
    m.fx.image.run_until_cancel = true;
    json resp;
    std::thread t([&] {
        resp = m.rpc(
            call_req(9, "hcs.image.generate",
                     {{"model", "sdmodel"}, {"prompt", "x"}, {"width", 64}, {"height", 64}, {"steps", 1}}));
    });
    ASSERT_TRUE(wait_for([&] { return m.fx.image.rendering.load(); }));
    json note = {{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", {{"requestId", 9}}}};
    (void)m.server.handle_line(note.dump());
    t.join();
    EXPECT_TRUE(wait_for([&] { return !m.fx.image.rendering.load(); }));
    EXPECT_TRUE(resp.is_null()) << "a cancelled request gets no response (MCP spec)";
}

TEST(McpImage, ToolTimeoutCancelsTheRender) {
    McpOptions o;
    o.call_timeout = std::chrono::milliseconds(300);
    Mcp m(o);
    m.fx.image.run_until_cancel = true;
    auto r = m.tool(1, "hcs.image.generate",
                    {{"model", "sdmodel"}, {"prompt", "x"}, {"width", 64}, {"height", 64}, {"steps", 1}});
    EXPECT_EQ(code_of(r), "timeout");
    EXPECT_TRUE(wait_for([&] { return !m.fx.image.rendering.load(); }));
}

TEST(McpImage, SchemaAdvertisesAllowLowQualityAndFlagIsForwarded) {
    for (const auto& t : tool_list(256))
        if (t["name"] == "hcs.image.generate" || t["name"] == "hcs.image.edit") {
            EXPECT_TRUE(t["inputSchema"]["properties"].contains("allow_low_quality"));
        }
    Mcp m;
    const auto path = m.fx.root / "sd15-mcp.gguf";
    test::write_file(path, test::fake_gguf());
    ModelManifest mm;
    mm.id = "sd15-mcp";
    mm.display_name = "sd15-mcp";
    mm.path = path.string();
    mm.format = ModelFormat::Gguf;
    mm.kind = ModelKind::Image;
    mm.size_bytes = 64;
    ASSERT_TRUE(m.fx.registry.add(mm).ok());
    json small = {{"model", "sd15-mcp"}, {"prompt", "x"}, {"width", 256}, {"height", 256}, {"steps", 2}};
    EXPECT_EQ(code_of(m.tool(1, "hcs.image.generate", small)), "invalid_argument");
    EXPECT_EQ(m.fx.image.generations.load(), 0);
    small["allow_low_quality"] = true;
    auto r = m.tool(2, "hcs.image.generate", small);
    ASSERT_FALSE(r["result"]["isError"].get<bool>()) << r.dump();
    EXPECT_TRUE(m.fx.image.last.allow_low_quality);
}

TEST(McpImage, LoraAndUpscaleFieldsAreInTheSchemaAndLoraListNeverLeaksPaths) {
    json tools = tool_list(256);
    bool listed = false;
    for (const auto& t : tools) {
        const std::string n = t["name"];
        if (n == "hcs.image.generate" || n == "hcs.image.edit") {
            const auto& props = t["inputSchema"]["properties"];
            ASSERT_TRUE(props.contains("loras")) << n;
            EXPECT_EQ(props["loras"]["maxItems"], 4);
            EXPECT_EQ(props["loras"]["items"]["additionalProperties"], false);
            EXPECT_FALSE(props["loras"]["items"]["properties"].contains("path"));
            EXPECT_EQ(props["upscale"]["enum"], json::array({"off", "4k"}));
        }
        if (n == "hcs.loras.list") {
            listed = true;
            EXPECT_TRUE(t["annotations"]["readOnlyHint"].get<bool>());
        }
    }
    EXPECT_TRUE(listed);
    Mcp m;
    auto r = m.tool(1, "hcs.loras.list");
    ASSERT_FALSE(r["result"]["isError"].get<bool>()) << r.dump();
    EXPECT_TRUE(r["result"]["structuredContent"]["loras"].empty());
}

TEST(McpImage, LoraRequestWithoutLibraryIsUnsupportedAndUpscaleWithoutRuntimeToo) {
    Mcp m;
    json base = {{"model", "sdmodel"}, {"prompt", "x"}, {"width", 256}, {"height", 256}, {"steps", 2}};
    json with_lora = base;
    with_lora["loras"] = json::array({json{{"id", "style"}}});
    EXPECT_EQ(code_of(m.tool(1, "hcs.image.generate", with_lora)), "unsupported");
    json with_up = base;
    with_up["upscale"] = "4k";
    EXPECT_EQ(code_of(m.tool(2, "hcs.image.generate", with_up)), "unsupported");
    EXPECT_EQ(m.fx.image.generations.load(), 0);
    json bad = base;
    bad["upscale"] = "8k";
    EXPECT_TRUE(m.tool(3, "hcs.image.generate", bad)["result"]["isError"].get<bool>());
}
