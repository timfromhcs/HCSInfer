// api::Runtime destruction contract (soak defect D-S1): destroying a Runtime directly - without the caller
// first calling queue->shutdown() / engine_queue->shutdown() - while a job is running must be safe.
// FakeTextEngine = plumbing only, not inference evidence.
#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <thread>

#include "fake_text_engine.hpp"
#include "hcs/api/runtime.hpp"
#include "hcs/logger.hpp"
#include "temp_dir.hpp"

using namespace hcs;
using namespace hcs::api;
using namespace std::chrono_literals;

namespace {

struct RtRig {
    test::TempDir dir;
    std::ostringstream sink;
    Logger logger{&sink, LogLevel::Debug};
    std::unique_ptr<Runtime> rt;
    test::FakeTextEngine* fake = nullptr;  // owned by rt; valid only until rt is destroyed
    std::string model_id;

    explicit RtRig(bool run_until_cancel, int delay_ms = 0) {
        const auto root = dir.path() / "models";
        test::write_file(root / "tiny.gguf", test::fake_gguf());
        RuntimeOptions o;
        o.state_dir = dir.path() / "state";
        std::filesystem::create_directories(o.state_dir);
        o.model_roots = {root.string()};
        auto eng = std::make_unique<test::FakeTextEngine>();
        eng->run_until_cancel = run_until_cancel;
        eng->delay_ms = delay_ms;
        fake = eng.get();
        o.engine = std::move(eng);
        auto r = Runtime::create(std::move(o), logger);
        EXPECT_TRUE(r.ok());
        if (!r)
            return;
        rt = std::move(r.value());
        (void)rt->registry.scan(root, rt->guard);
        auto models = rt->registry.list();
        if (!models.empty())
            model_id = models.front().id;
    }
};

ChatParams chat(const std::string& model) {
    ChatParams p;
    p.model = model;
    p.messages.push_back({"user", "hi", {}});
    return p;
}

void wait_state(ControlPlane& cp, JobId id, JobState want) {
    for (int i = 0; i < 400; ++i) {
        auto st = cp.job_status(id);
        if (st.ok() && st.value().state == want)
            return;
        std::this_thread::sleep_for(10ms);
    }
}

}  // namespace

TEST(RuntimeLifecycle, DestroyWithRunningChatJobDoesNotCrash) {
    for (int round = 0; round < 5; ++round) {
        RtRig rig(/*run_until_cancel=*/true);
        ASSERT_TRUE(rig.rt);
        ASSERT_FALSE(rig.model_id.empty());
        auto sub = rig.rt->control->submit_chat(chat(rig.model_id));
        ASSERT_TRUE(sub.ok()) << sub.error().message;
        wait_state(*rig.rt->control, sub.value().id, JobState::Running);
        for (int i = 0; i < 200 && !rig.fake->generating; ++i) std::this_thread::sleep_for(5ms);
        // Direct destruction on a watchdog thread: a hang is a failure, a crash fails the whole binary.
        auto fut = std::async(std::launch::async, [&] { rig.rt.reset(); });
        ASSERT_EQ(fut.wait_for(30s), std::future_status::ready) << "Runtime destruction hung";
    }
}

TEST(RuntimeLifecycle, DestroyWithRunningLoadJobDoesNotCrash) {
    RtRig rig(false);
    ASSERT_TRUE(rig.rt);
    auto j = rig.rt->control->submit_load(rig.model_id);
    ASSERT_TRUE(j.ok());
    rig.rt.reset();  // load may be queued, running or done: all three must be safe
    SUCCEED();
}

TEST(RuntimeLifecycle, DestroyIdleRuntimeIsClean) {
    RtRig rig(false);
    ASSERT_TRUE(rig.rt);
    rig.rt.reset();
    SUCCEED();
}

TEST(RuntimeLifecycle, ExplicitShutdownThenDestroyStillWorks) {
    RtRig rig(true);
    ASSERT_TRUE(rig.rt);
    auto sub = rig.rt->control->submit_chat(chat(rig.model_id));
    ASSERT_TRUE(sub.ok());
    wait_state(*rig.rt->control, sub.value().id, JobState::Running);
    rig.rt->queue->shutdown();
    rig.rt->engine_queue->shutdown();
    rig.rt.reset();  // shutdown is idempotent: the serve/mcp protocol must remain valid
    SUCCEED();
}
