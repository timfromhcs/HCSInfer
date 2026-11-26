#include <gtest/gtest.h>

#include <filesystem>

#include "hcs/model_registry.hpp"
#include "temp_dir.hpp"

using namespace hcs;
using hcs::test::TempDir;
using hcs::test::write_file;
namespace fs = std::filesystem;

namespace {
ModelManifest manifest(const std::string& id) {
    ModelManifest m;
    m.id = id;
    m.path = "X:\\" + id + ".gguf";
    m.format = ModelFormat::Gguf;
    m.kind = ModelKind::Text;
    return m;
}
}  // namespace

TEST(ModelId, Validation) {
    EXPECT_TRUE(is_valid_model_id("qwen3-0.6b-q4_k_m"));
    for (const char* bad : {"", "-x", ".x", "Upper", "has space", "a/b", "a\\b", "..", "é"}) {
        EXPECT_FALSE(is_valid_model_id(bad)) << bad;
    }
    EXPECT_FALSE(is_valid_model_id(std::string(65, 'a')));
    EXPECT_TRUE(is_valid_model_id(std::string(64, 'a')));
}

TEST(ModelId, MakeIdSanitises) {
    EXPECT_EQ(make_model_id("Qwen3 0.6B (Q4_K_M)"), "qwen3-0.6b--q4_k_m-");
    EXPECT_TRUE(is_valid_model_id(make_model_id("***")));
    EXPECT_TRUE(is_valid_model_id(make_model_id(std::string(200, 'Z'))));
    EXPECT_TRUE(is_valid_model_id(make_model_id("")));
}

TEST(ModelRegistry, AddGetListRemove) {
    ModelRegistry r;
    ASSERT_TRUE(r.add(manifest("b")));
    ASSERT_TRUE(r.add(manifest("a")));
    EXPECT_EQ(r.size(), 2u);
    auto list = r.list();
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0].id, "a");  // sorted
    EXPECT_EQ(r.get("a").value().display_name, "a");
    EXPECT_EQ(r.get("zzz").error().code, ErrorCode::NotFound);
    EXPECT_EQ(r.state("a").value(), ModelState::Discovered);
    ASSERT_TRUE(r.remove("a"));
    EXPECT_EQ(r.remove("a").error().code, ErrorCode::NotFound);
}

TEST(ModelRegistry, RejectsBadAndDuplicateIds) {
    ModelRegistry r;
    EXPECT_EQ(r.add(manifest("Bad Id")).error().code, ErrorCode::InvalidArgument);
    ASSERT_TRUE(r.add(manifest("x")));
    EXPECT_EQ(r.add(manifest("x")).error().code, ErrorCode::AlreadyExists);
}

TEST(ModelRegistry, LifecycleThroughRegistryAndRemovalGuard) {
    ModelRegistry r;
    ASSERT_TRUE(r.add(manifest("m")));
    for (auto s :
         {ModelState::Validating, ModelState::ReadyToLoad, ModelState::Loading, ModelState::Loaded}) {
        ASSERT_TRUE(r.transition("m", s));
    }
    EXPECT_EQ(r.remove("m").error().code, ErrorCode::InvalidState);  // loaded: refuse
    EXPECT_FALSE(r.transition("m", ModelState::Loading));            // illegal
    ASSERT_TRUE(r.transition("m", ModelState::Unloading));
    ASSERT_TRUE(r.transition("m", ModelState::Unloaded));
    EXPECT_TRUE(r.remove("m"));
    EXPECT_EQ(r.transition("m", ModelState::Loading).error().code, ErrorCode::NotFound);
}

TEST(ModelRegistry, FailMarksFailed) {
    ModelRegistry r;
    ASSERT_TRUE(r.add(manifest("m")));
    EXPECT_FALSE(r.fail("m", FailureKind::Recoverable, "too early"));  // Discovered cannot fail
    for (auto s : {ModelState::Validating, ModelState::ReadyToLoad, ModelState::Loading})
        ASSERT_TRUE(r.transition("m", s));
    ASSERT_TRUE(r.fail("m", FailureKind::NonRecoverable, "oom"));
    EXPECT_EQ(r.state("m").value(), ModelState::Failed);
}

TEST(ModelRegistry, JsonRoundTrip) {
    ModelRegistry a;
    auto m = manifest("model-1");
    m.size_bytes = 12345;
    m.architecture = "qwen3";
    ASSERT_TRUE(a.add(m));
    ModelRegistry b;
    ASSERT_TRUE(b.load_json(a.to_json()));
    auto got = b.get("model-1").value();
    EXPECT_EQ(got.size_bytes, 12345u);
    EXPECT_EQ(got.architecture, "qwen3");
    EXPECT_EQ(got.format, ModelFormat::Gguf);
    EXPECT_EQ(b.state("model-1").value(), ModelState::Discovered);
    EXPECT_TRUE(b.load_json(a.to_json()));  // idempotent
    EXPECT_EQ(b.size(), 1u);
}

TEST(ModelRegistry, LoadJsonRejectsGarbage) {
    ModelRegistry r;
    EXPECT_EQ(r.load_json("nope").error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(r.load_json(R"({"schema_version":9,"models":[]})").error().code, ErrorCode::Unsupported);
    EXPECT_EQ(r.load_json(R"({"schema_version":1,"models":[{"id":"x"}]})").error().code,
              ErrorCode::InvalidArgument);
    EXPECT_EQ(r.load_json(R"({"schema_version":1,"models":[{"id":"BAD ID","path":"p"}]})").error().code,
              ErrorCode::InvalidArgument);
}

TEST(ModelRegistryScan, FindsValidatesAndIsIdempotent) {
    TempDir t;
    write_file(t / "root" / "good.gguf", hcs::test::fake_gguf());
    write_file(t / "root" / "sub" / "Other Model.GGUF", hcs::test::fake_gguf(2));
    write_file(t / "root" / "fake.gguf", "this is not gguf at all");
    write_file(t / "root" / "future.gguf", hcs::test::fake_gguf(99));
    write_file(t / "root" / "tiny.gguf", "GG");
    write_file(t / "root" / "notes.txt", "ignore");
    PathGuard g;
    ASSERT_TRUE(g.add_root(t / "root"));
    ModelRegistry r;
    auto rep = r.scan(t / "root", g);
    ASSERT_TRUE(rep) << rep.error().message;
    EXPECT_EQ(rep.value().added, 2u);
    EXPECT_EQ(rep.value().skipped, 3u);
    EXPECT_EQ(r.size(), 2u);
    EXPECT_TRUE(r.get("good").ok());
    EXPECT_TRUE(r.get("other-model").ok());
    EXPECT_EQ(r.get("good").value().kind, ModelKind::Text);
    EXPECT_EQ(r.get("good").value().format, ModelFormat::Gguf);
    EXPECT_GT(r.get("good").value().size_bytes, 0u);

    auto again = r.scan(t / "root", g);
    ASSERT_TRUE(again);
    EXPECT_EQ(again.value().added, 0u);
    EXPECT_EQ(again.value().updated, 2u);
    EXPECT_EQ(r.size(), 2u);
}

TEST(ModelRegistryScan, NonModelFilesAreIgnoredWithoutNoiseAndModelsInDeepTreesAreStillFound) {
    TempDir t;
    for (int i = 0; i < 300; ++i)
        write_file(t / "root" / "venv" / "lib" / ("junk" + std::to_string(i) + ".py"), "x");
    write_file(t / "root" / "venv" / "lib" / "notes.GGUF.txt", "not a model");
    write_file(t / "root" / "a" / "b" / "c" / "deep.gguf", hcs::test::fake_gguf());
    write_file(t / "root" / "w" / "m.safetensors", "garbage that is no safetensors header");
    PathGuard g;
    ASSERT_TRUE(g.add_root(t / "root"));
    ModelRegistry r;
    auto rep = r.scan(t / "root", g);
    ASSERT_TRUE(rep) << rep.error().message;
    EXPECT_EQ(rep.value().added, 1u);
    EXPECT_EQ(rep.value().skipped, 1u);  // only the broken .safetensors; the 301 non-model files make no noise
    EXPECT_EQ(rep.value().messages.size(), 1u);
    EXPECT_TRUE(r.get("deep").ok());
}

TEST(ModelRegistryDigest, SetSha256ValidatesNormalisesAndSurvivesJsonRoundTrip) {
    TempDir t;
    write_file(t / "root" / "good.gguf", hcs::test::fake_gguf());
    PathGuard g;
    ASSERT_TRUE(g.add_root(t / "root"));
    ModelRegistry r;
    ASSERT_TRUE(r.scan(t / "root", g));
    EXPECT_EQ(r.set_sha256("good", "short").error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(r.set_sha256("good", std::string(63, 'a') + "g").error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(r.set_sha256("ghost", std::string(64, 'a')).error().code, ErrorCode::NotFound);
    ASSERT_TRUE(r.set_sha256("good", std::string(64, 'A')));
    EXPECT_EQ(r.get("good").value().sha256, std::string(64, 'a'));
    ModelRegistry b;
    ASSERT_TRUE(b.load_json(r.to_json()));
    EXPECT_EQ(b.get("good").value().sha256, std::string(64, 'a'));
}

TEST(ModelRegistryDigest, RescanKeepsTheDigestOfAnUnchangedFileAndDropsItWhenTheSizeChanged) {
    TempDir t;
    write_file(t / "root" / "good.gguf", hcs::test::fake_gguf());
    PathGuard g;
    ASSERT_TRUE(g.add_root(t / "root"));
    ModelRegistry r;
    ASSERT_TRUE(r.scan(t / "root", g));
    ASSERT_TRUE(r.set_sha256("good", std::string(64, 'b')));
    ASSERT_TRUE(r.scan(t / "root", g));  // same file: digest stays, and nothing was re-hashed
    EXPECT_EQ(r.get("good").value().sha256, std::string(64, 'b'));
    write_file(t / "root" / "good.gguf", hcs::test::fake_gguf() + "extra bytes");
    ASSERT_TRUE(r.scan(t / "root", g));
    EXPECT_TRUE(r.get("good").value().sha256.empty()) << "a changed file must not keep its old digest";
}

TEST(ModelRegistryScan, DuplicateStemsGetSuffixedIds) {
    TempDir t;
    write_file(t / "r" / "a" / "m.gguf", hcs::test::fake_gguf());
    write_file(t / "r" / "b" / "m.gguf", hcs::test::fake_gguf());
    PathGuard g;
    ASSERT_TRUE(g.add_root(t / "r"));
    ModelRegistry r;
    ASSERT_TRUE(r.scan(t / "r", g));
    EXPECT_EQ(r.size(), 2u);
    EXPECT_TRUE(r.get("m").ok());
    EXPECT_TRUE(r.get("m-2").ok());
}

TEST(ModelRegistryScan, SafetensorsHeaderChecked) {
    TempDir t;
    std::string good = "\x10";
    good.append(7, '\0');
    good.append(16, 'x');
    write_file(t / "r" / "ok.safetensors", good);
    std::string bad(8, '\xff');
    write_file(t / "r" / "bad.safetensors", bad);
    PathGuard g;
    ASSERT_TRUE(g.add_root(t / "r"));
    ModelRegistry r;
    auto rep = r.scan(t / "r", g);
    ASSERT_TRUE(rep);
    EXPECT_EQ(rep.value().added, 1u);
    EXPECT_EQ(rep.value().skipped, 1u);
    EXPECT_EQ(r.get("ok").value().format, ModelFormat::Safetensors);
}

TEST(ModelRegistryScan, DoesNotFollowLinksOutOfRoot) {
    TempDir t;
    write_file(t / "outside" / "evil.gguf", hcs::test::fake_gguf());
    write_file(t / "root" / "fine.gguf", hcs::test::fake_gguf());
    if (!hcs::test::make_dir_link(t / "root" / "escape", t / "outside"))
        GTEST_SKIP() << "no symlink/junction support";
    PathGuard g;
    ASSERT_TRUE(g.add_root(t / "root"));
    ModelRegistry r;
    auto rep = r.scan(t / "root", g);
    ASSERT_TRUE(rep) << rep.error().message;
    EXPECT_TRUE(r.get("fine").ok());
    EXPECT_FALSE(r.get("evil").ok());
    EXPECT_EQ(r.size(), 1u);
    EXPECT_GE(rep.value().skipped, 1u);
}

TEST(ModelRegistryScan, RootOutsideGuardIsRejected) {
    TempDir t;
    fs::create_directories(t / "root");
    fs::create_directories(t / "other");
    PathGuard g;
    ASSERT_TRUE(g.add_root(t / "root"));
    ModelRegistry r;
    auto rep = r.scan(t / "other", g);
    ASSERT_FALSE(rep);
    EXPECT_EQ(rep.error().code, ErrorCode::SecurityViolation);
}

TEST(ModelRegistryScan, NeverModifiesScannedFiles) {
    TempDir t;
    const auto f = t / "r" / "m.gguf";
    write_file(f, hcs::test::fake_gguf());
    const auto before = hcs::test::read_file(f);
    const auto stamp = fs::last_write_time(f);
    PathGuard g;
    ASSERT_TRUE(g.add_root(t / "r"));
    ModelRegistry r;
    ASSERT_TRUE(r.scan(t / "r", g));
    EXPECT_EQ(hcs::test::read_file(f), before);
    EXPECT_EQ(fs::last_write_time(f), stamp);
    std::size_t n = 0;
    for (auto& e : fs::recursive_directory_iterator(t / "r")) {
        (void)e;
        ++n;
    }
    EXPECT_EQ(n, 1u);
}
