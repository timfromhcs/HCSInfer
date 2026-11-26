// `HCSInfer image upscale`: argument validation, path rules and the "no runtime / no model" structured error.
// The real-model run lives in the wave-c/u1 evidence (needs onnxruntime.dll and a real .onnx).
#include <gtest/gtest.h>

#include <filesystem>
#include <sstream>

#include "hcs/cli/cli.hpp"
#include "hcs/upscale.hpp"
#include "temp_dir.hpp"

using namespace hcs;
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

std::string small_png() {
    upscale::Rgb8 img;
    img.width = 16;
    img.height = 8;
    img.pixels.assign(16 * 8 * 3, 100);
    auto enc = upscale::encode_png(img);
    EXPECT_TRUE(enc.ok());
    return std::string(enc.value().begin(), enc.value().end());
}

bool contains(const std::string& s, const std::string& n) {
    return s.find(n) != std::string::npos;
}

}  // namespace

TEST(CliUpscale, HelpDocumentsTheOptionsAndIsReachableFromImageHelp) {
    auto r = run({"image", "upscale", "--help"});
    EXPECT_EQ(r.code, 0);
    for (const char* flag : {"--input", "--output", "--tile", "--overlap", "--threads", "--provider",
                             "--target", "--upscale-dir", "--force", "--json", "Ctrl-C"})
        EXPECT_TRUE(contains(r.out, flag)) << flag;
    EXPECT_TRUE(contains(run({"image", "--help"}).out, "image upscale"));
}

TEST(CliUpscale, ArgumentErrorsAreInvalidArgumentExit2) {
    EXPECT_EQ(run({"image", "upscale"}).code, 2) << "--input required";
    EXPECT_EQ(run({"image", "upscale", "--input"}).code, 2) << "missing value";
    EXPECT_EQ(run({"image", "upscale", "--input", "a.png", "--bogus"}).code, 2);
    EXPECT_EQ(run({"image", "upscale", "--input", "a.png", "--provider", "gpu"}).code, 2);
    EXPECT_EQ(run({"image", "upscale", "--input", "a.png", "--threads", "0"}).code, 2);
    EXPECT_EQ(run({"image", "upscale", "--input", "a.png", "--tile", "abc"}).code, 2);
}

TEST(CliUpscale, PathRulesAreEnforcedBeforeAnyWork) {
    test::TempDir dir;
    const auto in = dir.path() / "in.png";
    test::write_file(in, small_png());
    auto traversal = run({"image", "upscale", "--input", (dir.path() / ".." / "x.png").string()});
    EXPECT_EQ(traversal.code, exit_code(ErrorCode::SecurityViolation)) << traversal.err;
    auto missing = run({"image", "upscale", "--input", (dir.path() / "nope.png").string()});
    EXPECT_EQ(missing.code, exit_code(ErrorCode::NotFound)) << missing.err;
    // The output exists and --force is absent: refused before the runtime is even looked for.
    const auto out = dir.path() / "out.png";
    test::write_file(out, "x");
    auto exists = run({"image", "upscale", "--input", in.string(), "--output", out.string()});
    EXPECT_EQ(exists.code, exit_code(ErrorCode::AlreadyExists)) << exists.err;
    EXPECT_EQ(test::read_file(out), "x") << "an existing file must not be touched";
    auto bad_dir = run({"image", "upscale", "--input", in.string(), "--output",
                        (dir.path() / "no-such-dir" / "o.png").string()});
    EXPECT_EQ(bad_dir.code, exit_code(ErrorCode::NotFound)) << bad_dir.err;
}

TEST(CliUpscale, MissingRuntimeOrModelIsUnsupportedExit10NamingWhatIsMissing) {
    test::TempDir dir;
    const auto in = dir.path() / "in.png";
    test::write_file(in, small_png());
    fs::create_directories(dir.path() / "up");
    auto r = run({"image", "upscale", "--input", in.string(), "--output", (dir.path() / "o.png").string(),
                  "--upscale-dir", (dir.path() / "up").string()});
    EXPECT_EQ(r.code, 10) << r.err;
    EXPECT_TRUE(contains(r.err, "onnxruntime.dll")) << r.err;
    EXPECT_TRUE(contains(r.err, "unsupported")) << r.err;
    EXPECT_FALSE(fs::exists(dir.path() / "o.png")) << "no output file is created on failure";
}
