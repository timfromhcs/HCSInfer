// `HCSInfer cache stats|clear`: only HCS-owned state-directory caches are ever touched - never model files,
// the registry, settings or train manifests - and nothing is removed without --yes.
#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>

#include "hcs/cli/cli.hpp"
#include "temp_dir.hpp"

using namespace hcs;
using hcs::test::TempDir;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct Run {
    int code;
    std::string out, err;
};
Run run(std::vector<std::string> args) {
    std::ostringstream o, e;
    const int c = cli::run(args, o, e);
    return {c, o.str(), e.str()};
}
bool contains(const std::string& s, const std::string& what) {
    return s.find(what) != std::string::npos;
}

void put(const fs::path& p, const std::string& content) {
    hcs::test::write_file(p, content);
}

Run cache(const fs::path& state, std::vector<std::string> args) {
    std::vector<std::string> a{"--state-dir", state.string(), "cache"};
    a.insert(a.end(), args.begin(), args.end());
    return run(a);
}

// What HCS really writes into the state directory: registry/settings/train manifests (state, never touched)
// plus, when a write was interrupted, atomic_write_file leftovers "<name>.tmp.<n>".
void populate_state(const fs::path& st) {
    put(st / "registry.json", "{}");
    put(st / "settings.json", "{}");
    put(st / "train" / "adapters" / "a1" / "manifest.json", "{}");
    put(st / "model-like.gguf", "GGUF-not-ours");  // nothing the cache command may ever touch
    put(st / "registry.json.tmp.3", "12345");      // orphaned temp file (5 bytes)
    put(st / "train" / "adapters" / "a1" / "manifest.json.tmp.0", "123");  // 3 bytes
    put(st / "notes.tmp.abc", "keep");                                     // not the atomic-write pattern
    put(st / "cache" / "k1.bin", "0123456789");                            // 10 bytes
    put(st / "cache" / "sub" / "k2.bin", "01234");                         // 5 bytes
    put(st / "scratch" / "s.bin", "01");                                   // 2 bytes
}

}  // namespace

TEST(CliCache, HelpListsSubcommandsAndIsNotPlanned) {
    auto h = run({"cache", "--help"});
    EXPECT_EQ(h.code, 0);
    EXPECT_TRUE(contains(h.out, "stats"));
    EXPECT_TRUE(contains(h.out, "clear"));
    EXPECT_TRUE(contains(h.out, "--yes"));
    EXPECT_FALSE(contains(h.out, "Planned"));
    EXPECT_TRUE(h.err.empty());
    EXPECT_EQ(run({"cache"}).code, 2);  // a subcommand is required
    EXPECT_EQ(run({"cache", "frob"}).code, 2);
    EXPECT_EQ(run({"cache", "stats", "--help"}).code, 0);
    EXPECT_EQ(run({"cache", "clear", "--help"}).code, 0);
    EXPECT_EQ(run({"cache", "stats", "--bogus"}).code, 2);
    EXPECT_EQ(run({"cache", "clear", "--bogus"}).code, 2);
}

TEST(CliCache, StatsOnMissingOrEmptyStateReportsZeroHonestly) {
    TempDir dir;
    for (const fs::path& st : {dir / "does-not-exist", dir.path()}) {
        auto r = cache(st, {"stats", "--json"});
        ASSERT_EQ(r.code, 0) << r.err;
        const json j = json::parse(r.out);
        EXPECT_EQ(j["schema_version"], 1);
        EXPECT_EQ(j["entries"], 0);
        EXPECT_EQ(j["bytes"], 0);
        EXPECT_TRUE(j.contains("locations"));
        EXPECT_TRUE(j.contains("note"));
    }
    auto text = cache(dir.path(), {"stats"});
    EXPECT_EQ(text.code, 0);
    EXPECT_TRUE(contains(text.out, "0"));
}

TEST(CliCache, StatsCountsOnlyHcsOwnedCacheEntries) {
    TempDir dir;
    const fs::path st = dir / "state";
    populate_state(st);
    auto r = cache(st, {"stats", "--json"});
    ASSERT_EQ(r.code, 0) << r.err;
    const json j = json::parse(r.out);
    // cache/: 2 files, 15 bytes; scratch/: 1 file, 2 bytes; temp files: 2 files, 8 bytes.
    EXPECT_EQ(j["entries"], 5);
    EXPECT_EQ(j["bytes"], 25);
    std::map<std::string, json> by_name;
    for (const auto& l : j["locations"]) by_name[l["name"].get<std::string>()] = l;
    ASSERT_TRUE(by_name.count("cache"));
    ASSERT_TRUE(by_name.count("scratch"));
    ASSERT_TRUE(by_name.count("orphaned_temp_files"));
    EXPECT_EQ(by_name["cache"]["entries"], 2);
    EXPECT_EQ(by_name["cache"]["bytes"], 15);
    EXPECT_EQ(by_name["scratch"]["entries"], 1);
    EXPECT_EQ(by_name["orphaned_temp_files"]["entries"], 2);
    EXPECT_EQ(by_name["orphaned_temp_files"]["bytes"], 8);
    // stats is read-only
    EXPECT_TRUE(fs::exists(st / "cache" / "k1.bin"));
    EXPECT_TRUE(fs::exists(st / "registry.json.tmp.3"));
}

TEST(CliCache, ClearRequiresYesAndRemovesNothingWithout) {
    TempDir dir;
    const fs::path st = dir / "state";
    populate_state(st);
    auto r = cache(st, {"clear"});
    EXPECT_EQ(r.code, 2);
    EXPECT_TRUE(contains(r.err, "--yes"));
    EXPECT_TRUE(fs::exists(st / "cache" / "k1.bin"));
    EXPECT_TRUE(fs::exists(st / "registry.json.tmp.3"));
    EXPECT_EQ(cache(st, {"clear", "--json"}).code, 2);
    EXPECT_TRUE(fs::exists(st / "scratch" / "s.bin"));
}

TEST(CliCache, ClearRemovesCachesAndTempFilesButNeverStateOrModels) {
    TempDir dir;
    const fs::path st = dir / "state";
    populate_state(st);
    auto r = cache(st, {"clear", "--yes", "--json"});
    ASSERT_EQ(r.code, 0) << r.err;
    const json j = json::parse(r.out);
    EXPECT_EQ(j["removed_entries"], 5);
    EXPECT_EQ(j["removed_bytes"], 25);
    EXPECT_TRUE(j["failed"].empty());
    EXPECT_FALSE(fs::exists(st / "cache" / "k1.bin"));
    EXPECT_FALSE(fs::exists(st / "cache" / "sub" / "k2.bin"));
    EXPECT_FALSE(fs::exists(st / "scratch" / "s.bin"));
    EXPECT_FALSE(fs::exists(st / "registry.json.tmp.3"));
    EXPECT_FALSE(fs::exists(st / "train" / "adapters" / "a1" / "manifest.json.tmp.0"));
    // preserved: state, adapters, a file that merely looks like a model, a non-matching temp name
    EXPECT_TRUE(fs::exists(st / "registry.json"));
    EXPECT_TRUE(fs::exists(st / "settings.json"));
    EXPECT_TRUE(fs::exists(st / "train" / "adapters" / "a1" / "manifest.json"));
    EXPECT_TRUE(fs::exists(st / "model-like.gguf"));
    EXPECT_TRUE(fs::exists(st / "notes.tmp.abc"));
    // idempotent: a second clear finds nothing
    auto again = cache(st, {"clear", "--yes", "--json"});
    ASSERT_EQ(again.code, 0);
    EXPECT_EQ(json::parse(again.out)["removed_entries"], 0);
    EXPECT_EQ(json::parse(cache(st, {"stats", "--json"}).out)["entries"], 0);
}

TEST(CliCache, ClearOnMissingStateDirectoryIsANoOp) {
    TempDir dir;
    auto r = cache(dir / "nope", {"clear", "--yes", "--json"});
    ASSERT_EQ(r.code, 0) << r.err;
    EXPECT_EQ(json::parse(r.out)["removed_entries"], 0);
    EXPECT_FALSE(fs::exists(dir / "nope"));  // and it does not create the directory
}

TEST(CliCache, ClearNeverFollowsLinksOutOfTheStateDirectory) {
    TempDir dir;
    const fs::path st = dir / "state";
    const fs::path outside = dir / "outside";
    put(outside / "precious.gguf", "precious model bytes");
    put(st / "cache" / "ok.bin", "123");
    fs::create_directories(st / "scratch");
    if (!hcs::test::make_dir_link(st / "scratch" / "escape", outside))
        GTEST_SKIP() << "cannot create a directory link on this machine";
    auto stats = cache(st, {"stats", "--json"});
    ASSERT_EQ(stats.code, 0) << stats.err;
    // the link target's file is never counted either
    EXPECT_EQ(json::parse(stats.out)["entries"], 1);
    auto r = cache(st, {"clear", "--yes", "--json"});
    EXPECT_TRUE(r.code == 0 || r.code == 8) << r.err;
    EXPECT_TRUE(fs::exists(outside / "precious.gguf"));  // the link target is untouched
    EXPECT_EQ(hcs::test::read_file(outside / "precious.gguf"), "precious model bytes");
    EXPECT_FALSE(fs::exists(st / "cache" / "ok.bin"));
    const json j = json::parse(r.out);
    EXPECT_FALSE(j["skipped"].empty());  // the link was reported as skipped, not silently followed
}
