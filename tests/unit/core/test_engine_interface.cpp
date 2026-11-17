#include <gtest/gtest.h>

#include "hcs/engine.hpp"

// Compile/behaviour contract for the engine interfaces: a fake adapter must be implementable
// using only the opaque value types (no upstream headers).
using namespace hcs;

namespace {
class FakeText final : public ITextEngine {
public:
    Result<void> load(const LoadRequest& r) override {
        if (r.model_path.empty()) return make_error(ErrorCode::InvalidArgument, "no path");
        loaded = true;
        return {};
    }
    Result<void> unload() override { loaded = false; return {}; }
    Result<JobId> generate(const TextRequest&) override {
        if (!loaded) return make_error(ErrorCode::InvalidState, "not loaded");
        return JobId{1};
    }
    Result<JobId> analyze_image(const VisionRequest&) override {
        return make_error(ErrorCode::Unsupported, "no projector");
    }
    EngineMetrics metrics() const override { return {}; }
    bool loaded = false;
};

class FakeImage final : public IImageEngine {
public:
    Result<void> load(const LoadRequest&) override { return {}; }
    Result<void> unload() override { return {}; }
    Result<JobId> generate(const ImageRequest&) override { return JobId{2}; }
    Result<JobId> edit(const ImageRequest& r) override {
        if (!r.init_image) return make_error(ErrorCode::InvalidArgument, "edit needs init image");
        return JobId{3};
    }
    EngineMetrics metrics() const override { return {}; }
};
}  // namespace

TEST(EngineInterface, TextAdapterContract) {
    FakeText e;
    ITextEngine& i = e;
    EXPECT_EQ(i.generate({}).error().code, ErrorCode::InvalidState);
    EXPECT_FALSE(i.load({}));
    LoadRequest lr;
    lr.model_path = "m.gguf";
    ASSERT_TRUE(i.load(lr));
    EXPECT_EQ(i.generate({}).value().value, 1u);
    EXPECT_EQ(i.analyze_image({}).error().code, ErrorCode::Unsupported);
    EXPECT_EQ(i.metrics().backend, "unknown");  // unknown stays unknown
    ASSERT_TRUE(i.unload());
}

TEST(EngineInterface, ImageAdapterContract) {
    FakeImage e;
    IImageEngine& i = e;
    EXPECT_TRUE(i.generate({}).value().valid());
    EXPECT_FALSE(i.edit({}));
    ImageRequest r;
    r.init_image = ImageInput{{1, 2, 3}};
    EXPECT_EQ(i.edit(r).value().value, 3u);
}

TEST(EngineInterface, DefaultsAreConservative) {
    LoadRequest lr;
    EXPECT_EQ(lr.backend, Backend::Auto);
    EXPECT_EQ(lr.context_tokens, 4096u);
    EXPECT_EQ(lr.gpu_layers, -1);
    EXPECT_EQ(to_string(JobId{7}), "job-7");
    EXPECT_FALSE(JobId{}.valid());
}
