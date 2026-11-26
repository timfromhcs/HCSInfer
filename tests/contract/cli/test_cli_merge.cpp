// `HCSInfer merge`: argument validation and one real end-to-end run on tiny synthetic safetensors files
// (the merge maths itself is covered against NumPy goldens in tests/unit/merge).
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <sstream>

#include "hcs/cli/cli.hpp"
#include "hcs/result.hpp"
#include "safetensors_builder.hpp"
#include "temp_dir.hpp"

using namespace hcs;
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

std::string model_file(const std::vector<float>& v) {
    return test::build_safetensors({test::f32_tensor("layer.weight", {2, 2}, v)});
}

std::vector<float> read_floats(const fs::path& p) {
    const std::string s = test::read_file(p);
    const std::uint64_t n = [&] {
        std::uint64_t x = 0;
        std::memcpy(&x, s.data(), 8);
        return x;
    }();
    std::vector<float> out(4);
    std::memcpy(out.data(), s.data() + 8 + n, 16);
    return out;
}

}  // namespace

TEST(CliMerge, HelpIsListedAndDocumentsTheLimits) {
    auto r = run({"merge", "--help"});
    EXPECT_EQ(r.code, 0);
    EXPECT_NE(r.out.find("quantized"), std::string::npos);
    EXPECT_NE(r.out.find("drive C:"), std::string::npos);
    EXPECT_NE(run({"--help"}).out.find("merge"), std::string::npos);
}

TEST(CliMerge, RejectsBadArguments) {
    test::TempDir td;
    test::write_file(td / "a.safetensors", model_file({1, 2, 3, 4}));
    test::write_file(td / "b.safetensors", model_file({3, 2, 1, 0}));
    const std::string a = (td / "a.safetensors").string(), b = (td / "b.safetensors").string();
    const std::string out = (td / "o.safetensors").string();
    EXPECT_EQ(run({"merge"}).code, exit_code(ErrorCode::InvalidArgument));
    EXPECT_EQ(run({"merge", "--method", "linear", "--base", a}).code, exit_code(ErrorCode::InvalidArgument));
    EXPECT_EQ(run({"merge", "--method", "average", "--base", a, "--model", b, "--out", out}).code,
              exit_code(ErrorCode::InvalidArgument));
    EXPECT_EQ(
        run({"merge", "--method", "linear", "--base", a, "--model", b, "--out", out, "--weights", "1,x,3"})
            .code,
        exit_code(ErrorCode::InvalidArgument));
    // slerp needs exactly one --model; wrong weight count; non-positive weights
    EXPECT_NE(run({"merge", "--method", "slerp", "--base", a, "--model", b, "--model", b, "--out", out}).code,
              0);
    EXPECT_NE(
        run({"merge", "--method", "linear", "--base", a, "--model", b, "--out", out, "--weights", "1"}).code,
        0);
    EXPECT_NE(
        run({"merge", "--method", "linear", "--base", a, "--model", b, "--out", out, "--weights", "1,-1"})
            .code,
        0);
    EXPECT_NE(run({"merge", "--method", "linear", "--base", a, "--model",
                   (td / "missing.safetensors").string(), "--out", out})
                  .code,
              0);
    EXPECT_FALSE(fs::exists(td / "o.safetensors")) << "no output may exist after a rejected merge";
}

TEST(CliMerge, LinearMergeWritesTheAverageAndAManifestAndRefusesToOverwrite) {
    test::TempDir td;
    test::write_file(td / "a.safetensors", model_file({1, 2, 3, 4}));
    test::write_file(td / "b.safetensors", model_file({3, 2, 1, 0}));
    const std::string out = (td / "merged.safetensors").string();
    auto r = run({"merge", "--method", "linear", "--base", (td / "a.safetensors").string(), "--model",
                  (td / "b.safetensors").string(), "--out", out, "--json"});
    ASSERT_EQ(r.code, 0) << r.err;
    const json j = json::parse(r.out);
    EXPECT_EQ(j["method"], "linear");
    EXPECT_EQ(j["tensors_merged"], 1);
    EXPECT_EQ(j["output_sha256"].get<std::string>().size(), 64u);
    const auto v = read_floats(out);
    EXPECT_FLOAT_EQ(v[0], 2.0f);
    EXPECT_FLOAT_EQ(v[1], 2.0f);
    EXPECT_FLOAT_EQ(v[2], 2.0f);
    EXPECT_FLOAT_EQ(v[3], 2.0f);
    EXPECT_TRUE(fs::exists(out + ".merge.json"));
    EXPECT_EQ(run({"merge", "--method", "linear", "--base", (td / "a.safetensors").string(), "--model",
                   (td / "b.safetensors").string(), "--out", out})
                  .code,
              exit_code(ErrorCode::AlreadyExists));
    // the output may never be one of the inputs, even with --force
    EXPECT_NE(run({"merge", "--method", "linear", "--base", (td / "a.safetensors").string(), "--model",
                   (td / "b.safetensors").string(), "--out", (td / "a.safetensors").string(), "--force"})
                  .code,
              0);
    EXPECT_EQ(read_floats(td / "a.safetensors")[0], 1.0f);
}
