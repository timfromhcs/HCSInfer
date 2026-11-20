#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <sstream>

#include "hcs/cli/cli.hpp"
#include "hcs/result.hpp"
#include "hcs/version.hpp"
#include "temp_dir.hpp"

using namespace hcs;
using hcs::test::TempDir;

namespace {
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
std::string root_help_text() {
    return run({"--help"}).out;
}
}  // namespace

TEST(Cli, HelpWorksAtEveryLevel) {
    const std::vector<std::vector<std::string>> levels = {
        {"--help"},
        {"-h"},
        {"help"},
        {"doctor", "--help"},
        {"models", "--help"},
        {"models", "list", "--help"},
        {"models", "scan", "--help"},
        {"models", "inspect", "--help"},
        {"chat", "--help"},
        {"vision", "--help"},
        {"image", "-h"},
    };
    for (const auto& l : levels) {
        auto r = run(l);
        EXPECT_EQ(r.code, 0) << l.front();
        EXPECT_TRUE(contains(r.out, "Usage") || contains(r.out, "HCSInfer")) << l.front();
        EXPECT_TRUE(r.err.empty());
    }
}

TEST(Cli, VersionAndNoArgs) {
    auto v = run({"--version"});
    EXPECT_EQ(v.code, 0);
    EXPECT_TRUE(contains(v.out, std::string("HCSInfer ") + HCS_VERSION_STRING));
    auto none = run({});
    EXPECT_EQ(none.code, exit_code(ErrorCode::InvalidArgument));
    EXPECT_TRUE(contains(none.err, "Usage"));
}

TEST(Cli, UnknownCommandIsInvalidArgument) {
    auto r = run({"frobnicate"});
    EXPECT_EQ(r.code, 2);
    EXPECT_TRUE(contains(r.err, "unknown command"));
    EXPECT_EQ(run({"models", "frob"}).code, 2);
    EXPECT_EQ(run({"doctor", "--bogus"}).code, 2);
    EXPECT_EQ(run({"--state-dir"}).code, 2);
}

TEST(Cli, PlannedCommandsReturnUnsupported) {
    // `tune` is deliberately not implemented and must never be faked: exit 10, help says Planned.
    auto r = run({"tune"});
    EXPECT_EQ(r.code, 10);
    EXPECT_TRUE(contains(r.err, "planned"));
    EXPECT_EQ(run({"tune", "--device", "radeon-680m"}).code, 10);
    auto h = run({"tune", "--help"});
    EXPECT_EQ(h.code, 0);
    EXPECT_TRUE(contains(h.out, "Planned"));
    EXPECT_TRUE(contains(root_help_text(), "tune"));
    EXPECT_TRUE(contains(root_help_text(), "Planned"));
    EXPECT_EQ(run({"models", "load", "x"}).code, 10);
    EXPECT_EQ(run({"models", "unload"}).code, 10);
}

TEST(Cli, BenchmarkHelpAndArgumentValidationBeforeLoadingAnything) {
    EXPECT_EQ(run({"benchmark", "--help"}).code, 0);
    EXPECT_TRUE(contains(run({"benchmark", "--help"}).out, "schema_version 1"));
    EXPECT_EQ(run({"benchmark"}).code, 2);  // --suite required
    EXPECT_EQ(run({"benchmark", "--suite", "bogus", "--model", "x"}).code, 2);
    EXPECT_EQ(run({"benchmark", "--suite", "quick"}).code, 2);  // --model required
    // 'full' is implemented now: it passes argument validation and then fails to resolve the model (7), or
    // reports that this build has no engine (10) - never "planned".
    {
        const auto full = run({"benchmark", "--suite", "full", "--model", "does-not-exist"});
        EXPECT_TRUE(full.code == 7 || full.code == 10) << full.code;
        EXPECT_FALSE(contains(full.err, "planned"));
    }
    EXPECT_TRUE(contains(run({"benchmark", "--help"}).out, "--sweep-sizes"));
    EXPECT_TRUE(contains(run({"benchmark", "--help"}).out, "full"));
    EXPECT_EQ(run({"benchmark", "--suite", "quick", "--model", "x", "--sweep-sizes", "128"}).code, 2);
    EXPECT_EQ(run({"benchmark", "--suite", "full", "--model", "x", "--sweep-sizes", "8"}).code, 2);
    EXPECT_EQ(run({"benchmark", "--suite", "full", "--model", "x", "--sweep-sizes", "128,,256"}).code, 2);
    EXPECT_EQ(run({"benchmark", "--suite", "full", "--model", "x", "--sweep-sizes", "abc"}).code, 2);
    EXPECT_EQ(
        run({"benchmark", "--suite", "full", "--model", "x", "--sweep-sizes", "16,16,16,16,16,16,16,16,16"})
            .code,
        2);
    EXPECT_EQ(run({"benchmark", "--suite", "quick", "--model", "x", "--iterations", "0"}).code, 2);
    EXPECT_EQ(run({"benchmark", "--suite", "quick", "--model", "x", "--backend", "gpu"}).code, 2);
    EXPECT_EQ(run({"benchmark", "--suite", "quick", "--model"}).code, 2);
    EXPECT_TRUE(contains(run({"benchmark", "--help"}).out, "--load-mode"));
    EXPECT_EQ(run({"benchmark", "--suite", "quick", "--model", "x", "--load-mode", "bogus"}).code,
              7);  // no such file
    {
        // An existing file whose load mode is invalid: the engine rejects the mode before touching the file,
        // so this is a deterministic InvalidArgument (exit 2) and never a "not a GGUF" model error.
        TempDir dir;
        const auto f = dir / "not-a-model.gguf";
        hcs::test::write_file(f, "not a gguf");
        EXPECT_EQ(run({"benchmark", "--suite", "quick", "--model", f.string(), "--load-mode", "bogus"}).code,
                  2);
    }
    const auto missing = run({"benchmark", "--suite", "quick", "--model", "does-not-exist"});
    EXPECT_TRUE(missing.code == 7 || missing.code == 10) << missing.code;  // not_found / no engine
}

TEST(Cli, ImplementedTransportsHaveHelpAndValidateArguments) {
    EXPECT_EQ(run({"serve", "--help"}).code, 0);
    EXPECT_EQ(run({"mcp"}).code, 2);  // missing transport is invalid, not "planned"
    EXPECT_EQ(run({"mcp", "frob"}).code, 2);
    EXPECT_EQ(run({"image"}).code, 2);
    EXPECT_TRUE(contains(run({"image"}).err, "Usage"));
    EXPECT_EQ(run({"image", "frob"}).code, 2);
    EXPECT_EQ(run({"image", "generate"}).code, 2);  // --model required
    EXPECT_EQ(run({"image", "generate", "--model", "m", "--prompt", "p", "--width", "abc"}).code, 2);
    EXPECT_EQ(run({"image", "generate", "--model", "m"}).code, 2);               // --prompt required
    EXPECT_EQ(run({"image", "edit", "--model", "m", "--prompt", "p"}).code, 2);  // --input required
    const auto missing = run({"image", "generate", "--model", "does-not-exist", "--prompt", "p"});
    EXPECT_TRUE(missing.code == 7 || missing.code == 10) << missing.code;
}

TEST(Cli, ChatValidatesArgumentsBeforeLoadingAnything) {
    EXPECT_EQ(run({"chat"}).code, 2);  // --model required
    EXPECT_EQ(run({"chat", "--model", "x", "--backend", "gpu"}).code, 2);
    EXPECT_EQ(run({"chat", "--model", "x", "--ctx", "abc"}).code, 2);
    EXPECT_EQ(run({"chat", "--model"}).code, 2);
    auto r = run({"chat", "--model", "does-not-exist", "--prompt", "hi"});
    EXPECT_TRUE(r.code == 7 || r.code == 10) << r.code;  // not_found (llama build) / unsupported (no engine)
}

TEST(Cli, VisionHelpAndArgumentValidationBeforeLoadingAnything) {
    auto h = run({"vision", "--help"});
    EXPECT_EQ(h.code, 0);
    EXPECT_TRUE(contains(h.out, "--mmproj"));
    EXPECT_TRUE(contains(h.out, "--image"));
    const int no_args = run({"vision"}).code;  // --model required (2) / no llama engine in this build (10)
    EXPECT_TRUE(no_args == 2 || no_args == 10) << no_args;
    const int no_image = run({"vision", "--model", "x"}).code;
    EXPECT_TRUE(no_image == 2 || no_image == 10) << no_image;
    TempDir d;
    test::write_file(d / "pic.png", "not really a png");
    // missing/invalid pieces are reported as errors, never "planned"
    const auto missing_image =
        run({"vision", "--model", "x", "--image", (d / "nope.png").string(), "--prompt", "p"});
    EXPECT_TRUE(missing_image.code == 7 || missing_image.code == 10) << missing_image.code;
    EXPECT_FALSE(contains(missing_image.err, "planned"));
    const auto traversal =
        run({"vision", "--model", "x", "--image", (d / ".." / "pic.png").string(), "--prompt", "p"});
    EXPECT_TRUE(traversal.code == 9 || traversal.code == 10) << traversal.code;
    const auto no_model =
        run({"vision", "--model", "does-not-exist", "--image", (d / "pic.png").string(), "--prompt", "p"});
    EXPECT_TRUE(no_model.code == 7 || no_model.code == 10) << no_model.code;
}

TEST(Cli, DoctorJsonIsValidAndNeverGuesses) {
    auto r = run({"doctor", "--json"});
    ASSERT_EQ(r.code, 0) << r.err;
    auto j = nlohmann::json::parse(r.out, nullptr, false);
    ASSERT_FALSE(j.is_discarded()) << r.out;
    EXPECT_EQ(j["schema_version"], 1);
#ifndef HCS_HAVE_HARDWARE
    EXPECT_EQ(j["hardware_module"], "absent");
    EXPECT_EQ(j["cpu"], "unknown");
    EXPECT_EQ(j["gpu"], "unknown");
    EXPECT_EQ(j["ram_bytes"], "unknown");
#endif
}

TEST(Cli, DoctorTextWorks) {
    auto r = run({"doctor"});
    EXPECT_EQ(r.code, 0) << r.err;
    EXPECT_FALSE(r.out.empty());
}

TEST(Cli, ModelsScanListInspectRoundTrip) {
    TempDir t;
    hcs::test::write_file(t / "models" / "tiny-model.gguf", hcs::test::fake_gguf());
    hcs::test::write_file(t / "models" / "broken.gguf", "garbage");
    const std::string state = (t / "state").string();
    const std::string dir = (t / "models").string();

    auto empty = run({"--state-dir", state, "models", "list"});
    EXPECT_EQ(empty.code, 0);
    EXPECT_TRUE(contains(empty.out, "No models registered"));

    auto scan = run({"--state-dir", state, "models", "scan", dir, "--json"});
    ASSERT_EQ(scan.code, 0) << scan.err;
    auto sj = nlohmann::json::parse(scan.out);
    EXPECT_EQ(sj["added"], 1);
    EXPECT_EQ(sj["skipped"], 1);

    auto list = run({"--state-dir", state, "models", "list"});
    EXPECT_EQ(list.code, 0);
    EXPECT_TRUE(contains(list.out, "tiny-model"));
    EXPECT_TRUE(contains(list.out, "gguf"));

    auto lj = run({"--state-dir", state, "models", "list", "--json"});
    auto arr = nlohmann::json::parse(lj.out);
    ASSERT_EQ(arr["models"].size(), 1u);
    EXPECT_EQ(arr["models"][0]["id"], "tiny-model");

    auto ins = run({"--state-dir", state, "models", "inspect", "tiny-model"});
    EXPECT_EQ(ins.code, 0);
    EXPECT_EQ(nlohmann::json::parse(ins.out)["format"], "gguf");

    EXPECT_EQ(run({"--state-dir", state, "models", "inspect", "nope"}).code, 7);

    auto again = run({"--state-dir", state, "models", "scan", dir, "--json"});
    ASSERT_EQ(again.code, 0);
    EXPECT_EQ(nlohmann::json::parse(again.out)["added"], 0);
    EXPECT_EQ(nlohmann::json::parse(again.out)["updated"], 1);
}

TEST(Cli, ScanErrors) {
    TempDir t;
    const std::string state = (t / "state").string();
    EXPECT_EQ(run({"--state-dir", state, "models", "scan"}).code, 2);
    EXPECT_EQ(run({"--state-dir", state, "models", "scan", (t / "missing").string()}).code, 7);
    EXPECT_EQ(run({"--state-dir", state, "models", "scan", (t / "a" / ".." / "b").string()}).code, 9);
}

TEST(Cli, CorruptRegistryIsReportedNotCrashed) {
    TempDir t;
    hcs::test::write_file(t / "state" / "registry.json", "{{{");
    auto r = run({"--state-dir", (t / "state").string(), "models", "list"});
    EXPECT_EQ(r.code, 2);
    EXPECT_TRUE(contains(r.err, "error:"));
}

// --------------------------------------------------------------------------------------------
// HCS Train (ADR-012): dataset validation, eval gate, adapter bookkeeping and the pipeline.
// --------------------------------------------------------------------------------------------
namespace {

std::string hex64(char c) {
    return std::string(64, c);
}

nlohmann::json eval_report(const std::string& eval_id, double heldout, char set = 'd') {
    return {{"schema_version", 1},
            {"eval_id", eval_id},
            {"set_sha256", hex64(set)},
            {"heldout", {{"samples", 100}, {"score", heldout}}},
            {"regression",
             nlohmann::json::array({nlohmann::json{{"name", "coding"}, {"samples", 50}, {"score", 0.9}},
                                    nlohmann::json{{"name", "tool_use"}, {"samples", 50}, {"score", 0.9}}})}};
}

nlohmann::json pipeline_input(const std::string& id, double base_heldout, double cand_heldout) {
    return {{"adapter_id", id},
            {"base_model_sha256", hex64('a')},
            {"adapter_sha256", hex64('b')},
            {"artifact_path", "D:/models/adapters/" + id + ".gguf"},
            {"dataset_manifest_sha256", hex64('c')},
            {"hyperparameters", {{"rank", "8"}}},
            {"baseline", eval_report("agentic-test", base_heldout)},
            {"candidate", eval_report("agentic-test", cand_heldout)}};
}

const char* kValidRecord =
    R"({"id":"r1","licence":"MIT","source":"unit-test","generator":"human",)"
    R"("messages":[{"role":"user","content":"hi"},{"role":"assistant","content":"hello"}]})";

}  // namespace

TEST(Cli, TrainHelpAndArgumentValidation) {
    for (const std::vector<std::string>& lvl :
         std::vector<std::vector<std::string>>{{"train", "--help"},
                                               {"train"},
                                               {"train", "validate", "--help"},
                                               {"train", "gate", "--help"},
                                               {"train", "adapter", "--help"},
                                               {"train", "pipeline", "--help"}}) {
        auto r = run(lvl);
        if (lvl == std::vector<std::string>{"train"}) {
            EXPECT_EQ(r.code, 2);
        } else {
            EXPECT_EQ(r.code, 0);
            EXPECT_TRUE(contains(r.out, "Usage")) << lvl.back();
        }
    }
    EXPECT_EQ(run({"train", "frob"}).code, 2);
    EXPECT_EQ(run({"train", "adapter", "frob"}).code, 2);
    EXPECT_EQ(run({"train", "validate"}).code, 2);  // --dataset required
    EXPECT_EQ(run({"train", "gate", "--baseline", "x"}).code, 2);
    EXPECT_EQ(run({"train", "pipeline"}).code, 2);  // --input required
}

TEST(Cli, TrainValidateReportsAcceptedAndRejected) {
    TempDir t;
    const auto dataset = t / "data.jsonl";
    hcs::test::write_file(dataset, std::string(kValidRecord) + "\n" +
                                       R"({"id":"r2","source":"x","generator":"human","messages":[]})" +
                                       "\n");
    auto r = run({"train", "validate", "--dataset", dataset.string(), "--json"});
    ASSERT_EQ(r.code, 0) << r.err;
    auto j = nlohmann::json::parse(r.out);
    EXPECT_EQ(j["accepted"], 1);
    EXPECT_EQ(j["rejected"], 1);
    EXPECT_EQ(j["clean"], false);
    EXPECT_FALSE(j["issues"].empty());

    auto text = run({"train", "validate", "--dataset", dataset.string()});
    EXPECT_EQ(text.code, 0);
    EXPECT_TRUE(contains(text.out, "accepted:"));

    EXPECT_EQ(run({"train", "validate", "--dataset", (t / "missing.jsonl").string()}).code, 7);
}

TEST(Cli, TrainGateKeepsRollsBackAndRejectsIncomparableSets) {
    TempDir t;
    const auto base = t / "base.json";
    const auto better = t / "better.json";
    const auto worse = t / "worse.json";
    const auto other = t / "other.json";
    hcs::test::write_file(base, eval_report("agentic-test", 0.50).dump());
    hcs::test::write_file(better, eval_report("agentic-test", 0.60).dump());
    hcs::test::write_file(worse, eval_report("agentic-test", 0.45).dump());
    hcs::test::write_file(other, eval_report("other-suite", 0.60).dump());

    auto keep = run({"train", "gate", "--baseline", base.string(), "--candidate", better.string(), "--json"});
    ASSERT_EQ(keep.code, 0) << keep.err;
    EXPECT_EQ(nlohmann::json::parse(keep.out)["decision"], "keep");

    auto rollback =
        run({"train", "gate", "--baseline", base.string(), "--candidate", worse.string(), "--json"});
    ASSERT_EQ(rollback.code, 0) << rollback.err;
    EXPECT_EQ(nlohmann::json::parse(rollback.out)["decision"], "rollback");

    // Different evaluation suites are not comparable: the gate fails closed.
    auto mix = run({"train", "gate", "--baseline", base.string(), "--candidate", other.string(), "--json"});
    ASSERT_EQ(mix.code, 0) << mix.err;
    EXPECT_EQ(nlohmann::json::parse(mix.out)["decision"], "invalid");

    EXPECT_EQ(
        run({"train", "gate", "--baseline", base.string(), "--candidate", better.string(), "--min-gain", "2"})
            .code,
        2);
}

TEST(Cli, TrainPipelinePromotesOnKeepAndRollsBack) {
    TempDir t;
    const std::string state = (t / "state").string();
    const auto good = t / "good.json";
    const auto bad = t / "bad.json";
    hcs::test::write_file(good, pipeline_input("keep-1", 0.50, 0.60).dump());
    hcs::test::write_file(bad, pipeline_input("roll-1", 0.60, 0.55).dump());

    auto promoted = run({"--state-dir", state, "train", "pipeline", "--input", good.string(), "--json"});
    ASSERT_EQ(promoted.code, 0) << promoted.err;
    auto pj = nlohmann::json::parse(promoted.out);
    EXPECT_EQ(pj["decision"], "keep");
    EXPECT_TRUE(pj["installed"].get<bool>());
    EXPECT_TRUE(pj["activated"].get<bool>());

    auto recorded = run({"--state-dir", state, "train", "pipeline", "--input", bad.string(), "--json"});
    ASSERT_EQ(recorded.code, 0) << recorded.err;
    EXPECT_EQ(nlohmann::json::parse(recorded.out)["activated"], false);

    auto list = run({"--state-dir", state, "train", "adapter", "list", "--json"});
    ASSERT_EQ(list.code, 0) << list.err;
    auto arr = nlohmann::json::parse(list.out);
    ASSERT_EQ(arr.size(), 2u);
    bool keep_active = false;
    bool roll_inactive = false;
    for (const auto& a : arr) {
        if (a["adapter_id"] == "keep-1" && a["active"] == true)
            keep_active = true;
        if (a["adapter_id"] == "roll-1" && a["active"] == false)
            roll_inactive = true;
    }
    EXPECT_TRUE(keep_active);
    EXPECT_TRUE(roll_inactive);

    // Activating an adapter the gate did not keep is refused (fail closed).
    EXPECT_EQ(run({"--state-dir", state, "train", "adapter", "activate", "roll-1"}).code, 11);

    auto rollback = run({"--state-dir", state, "train", "adapter", "rollback", "--base", hex64('a')});
    EXPECT_EQ(rollback.code, 0) << rollback.err;
    auto after = run({"--state-dir", state, "train", "adapter", "list", "--json"});
    for (const auto& a : nlohmann::json::parse(after.out)) {
        EXPECT_FALSE(a["active"].get<bool>());
    }

    auto shown = run({"--state-dir", state, "train", "adapter", "show", "keep-1"});
    EXPECT_EQ(shown.code, 0);
    EXPECT_EQ(nlohmann::json::parse(shown.out)["adapter_id"], "keep-1");
    EXPECT_EQ(run({"--state-dir", state, "train", "adapter", "show", "nope"}).code, 7);
}

TEST(Cli, TrainPipelineRejectsIncomparableInputWithoutWriting) {
    TempDir t;
    const std::string state = (t / "state").string();
    auto input = pipeline_input("mismatch", 0.50, 0.60);
    input["candidate"] = eval_report("other-suite", 0.60);
    const auto file = t / "mismatch.json";
    hcs::test::write_file(file, input.dump());
    auto r = run({"--state-dir", state, "train", "pipeline", "--input", file.string()});
    EXPECT_EQ(r.code, 2);
    EXPECT_TRUE(contains(r.err, "comparable"));
    auto list = run({"--state-dir", state, "train", "adapter", "list", "--json"});
    EXPECT_EQ(nlohmann::json::parse(list.out).size(), 0u);
}
