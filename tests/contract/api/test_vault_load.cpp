// Opt-in Model Vault stage in the control-plane load path. FakeTextEngine = plumbing only (not inference
// evidence): these tests prove ORDER (prefetch before the engine sees the file), ROLLBACK (a failed prefetch
// never reaches the engine and leaves the model retryable), the default-off behaviour and the real
// storage-backed wiring of `Runtime` (real file, real digest).
#include <gtest/gtest.h>

#include <atomic>
#include <nlohmann/json.hpp>

#include "api_fixture.hpp"
#include "hcs/api/runtime.hpp"
#include "hcs/storage/sha256.hpp"

using namespace hcs;
using namespace hcs::api;
using nlohmann::json;

TEST(VaultLoad, DefaultIsOffAndReportsNoVaultSection) {
    test::ApiFixture fx;
    ASSERT_TRUE(fx.cp->load_model("tiny").ok());
    auto rt = fx.cp->runtime_json();
    EXPECT_EQ(rt["vault_prefetch"], false);
    EXPECT_FALSE(rt.contains("vault"));
}

TEST(VaultLoad, HookRunsBeforeTheEngineWithTheResolvedPathAndManifestDigest) {
    std::atomic<int> calls{0};
    std::string seen_path, seen_digest;
    test::FakeTextEngine* eng = nullptr;
    int loads_at_hook = -1;
    ControlPlaneOptions o;
    o.model_prefetch = [&](const std::filesystem::path& p, const std::string& d, const CancellationToken&) {
        ++calls;
        seen_path = p.string();
        seen_digest = d;
        loads_at_hook = eng ? eng->loads.load() : -1;
        return Result<json>(json{{"bytes", 42}});
    };
    test::ApiFixture fx(true, o);
    eng = &fx.engine;
    ASSERT_TRUE(fx.cp->load_model("tiny").ok());
    EXPECT_EQ(calls.load(), 1);
    EXPECT_EQ(loads_at_hook, 0) << "the engine must not have loaded yet when the vault stage runs";
    EXPECT_EQ(fx.engine.loads.load(), 1);
    EXPECT_EQ(std::filesystem::path(seen_path).filename(), "tiny.gguf");
    EXPECT_EQ(seen_path, fx.engine.last_path) << "the engine gets exactly the path the vault stage read";
    EXPECT_TRUE(seen_digest.empty());  // never hashed => nothing to compare against
    auto rt = fx.cp->runtime_json();
    EXPECT_EQ(rt["vault_prefetch"], true);
    EXPECT_EQ(rt["vault"]["model"]["bytes"], 42);
}

TEST(VaultLoad, FailedPrefetchNeverReachesTheEngineAndTheModelStaysRetryable) {
    bool fail = true;
    ControlPlaneOptions o;
    o.model_prefetch = [&](const std::filesystem::path&, const std::string&, const CancellationToken&) {
        if (fail)
            return Result<json>(
                Error(ErrorCode::ModelError, "model file digest mismatch: expected a, got b"));
        return Result<json>(json::object());
    };
    test::ApiFixture fx(true, o);
    auto r = fx.cp->load_model("tiny");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::ModelError);
    EXPECT_EQ(fx.engine.loads.load(), 0);
    EXPECT_FALSE(fx.cp->loaded_model().has_value());
    EXPECT_EQ(fx.budget.reserved(), 0u) << "reservations must be rolled back";
    fail = false;
    ASSERT_TRUE(fx.cp->load_model("tiny").ok()) << "a failed vault stage must be recoverable";
    EXPECT_EQ(fx.engine.loads.load(), 1);
}

TEST(VaultLoad, CancelledPrefetchIsCancelledNotAFailureOfTheModel) {
    ControlPlaneOptions o;
    o.model_prefetch = [&](const std::filesystem::path&, const std::string&, const CancellationToken&) {
        return Result<json>(Error(ErrorCode::Cancelled, "cancelled"));
    };
    test::ApiFixture fx(true, o);
    auto r = fx.cp->load_model("tiny");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Cancelled);
    EXPECT_EQ(fx.engine.loads.load(), 0);
}

namespace {
struct RtRig {
    test::TempDir dir;
    std::ostringstream sink;
    Logger logger{&sink, LogLevel::Debug};
    std::unique_ptr<Runtime> rt;
    test::FakeTextEngine* fake = nullptr;
    std::filesystem::path model_file;

    explicit RtRig(bool vault, const std::string& manifest_sha = "") {
        const auto root = dir.path() / "models";
        model_file = root / "tiny.gguf";
        test::write_file(model_file, test::fake_gguf());
        RuntimeOptions o;
        o.state_dir = dir.path() / "state";
        std::filesystem::create_directories(o.state_dir);
        if (!manifest_sha.empty()) {  // a registry written earlier (e.g. by `models hash`) carries the digest
            json reg = {
                {"schema_version", 1},
                {"models", json::array({json{{"id", "tiny"},
                                             {"path", std::filesystem::canonical(model_file).string()},
                                             {"format", "gguf"},
                                             {"kind", "text"},
                                             {"size_bytes", std::filesystem::file_size(model_file)},
                                             {"sha256", manifest_sha}}})}};
            test::write_file(o.state_dir / "registry.json", reg.dump());
        }
        o.model_roots = {root.string()};
        o.vault_prefetch = vault;
        o.vault_buffers = 2;
        o.vault_chunk_mib = 1;
        auto eng = std::make_unique<test::FakeTextEngine>();
        fake = eng.get();
        o.engine = std::move(eng);
        auto r = Runtime::create(std::move(o), logger);
        EXPECT_TRUE(r.ok());
        if (!r)
            return;
        rt = std::move(r.value());
        (void)rt->registry.scan(root, rt->guard);
    }
    std::string id() const { return rt->registry.list().front().id; }
};
}  // namespace

TEST(VaultLoad, RuntimeWiresTheRealStorageReaderAndReportsTheRealDigest) {
    RtRig rig(true);
    ASSERT_TRUE(rig.rt);
    ASSERT_TRUE(rig.rt->control->load_model(rig.id()).ok());
    const std::string bytes = test::read_file(rig.model_file);
    const auto want = storage::to_hex(storage::sha256(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size())));
    auto rt = rig.rt->control->runtime_json();
    ASSERT_TRUE(rt.contains("vault"));
    EXPECT_EQ(rt["vault"]["model"]["sha256"], want);
    EXPECT_EQ(rt["vault"]["model"]["bytes"], bytes.size());
    EXPECT_EQ(rt["vault"]["model"]["verified_against_manifest"], false);  // the manifest has no digest
    EXPECT_LE(rt["vault"]["model"]["buffer_bytes"].get<std::uint64_t>(), 2u * 2u * (1u << 20));
}

TEST(VaultLoad, RuntimeAcceptsAFileWhoseDigestMatchesTheManifest) {
    test::TempDir probe;
    test::write_file(probe / "x.gguf", test::fake_gguf());
    const std::string bytes = test::read_file(probe / "x.gguf");
    const auto digest = storage::to_hex(storage::sha256(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size())));
    RtRig rig(true, digest);
    ASSERT_TRUE(rig.rt);
    ASSERT_TRUE(rig.rt->control->load_model(rig.id()).ok());
    EXPECT_EQ(rig.rt->control->runtime_json()["vault"]["model"]["verified_against_manifest"], true);
}

TEST(VaultLoad, RuntimeRefusesAFileWhoseDigestDiffersFromTheManifest) {
    RtRig rig(true, std::string(64, '0'));
    ASSERT_TRUE(rig.rt);
    ASSERT_EQ(rig.rt->registry.list().size(), 1u);
    ASSERT_EQ(rig.rt->registry.list().front().sha256, std::string(64, '0'));
    auto r = rig.rt->control->load_model(rig.id());
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::ModelError);
    EXPECT_NE(r.error().message.find("mismatch"), std::string::npos);
    EXPECT_EQ(rig.fake->loads.load(), 0);
}

TEST(VaultLoad, RuntimeWithoutTheFlagHasNoVaultStage) {
    RtRig rig(false);
    ASSERT_TRUE(rig.rt);
    ASSERT_TRUE(rig.rt->control->load_model(rig.id()).ok());
    EXPECT_EQ(rig.rt->control->runtime_json()["vault_prefetch"], false);
}

TEST(VaultLoad, RuntimeRejectsOutOfRangeVaultSettings) {
    test::TempDir dir;
    std::ostringstream sink;
    Logger logger{&sink, LogLevel::Debug};
    RuntimeOptions o;
    o.state_dir = dir.path();
    o.vault_prefetch = true;
    o.vault_buffers = 99;
    auto r = Runtime::create(std::move(o), logger);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
}
