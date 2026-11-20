// OOM injection at the allocator boundary (AGENTS.md 10.3 / 17.5): a refused allocation - whether from the
// memory-budget ledger (fault hook), from the engine returning OutOfMemory, or from std::bad_alloc escaping
// an engine - must surface as a structured OutOfMemory error, roll back every partial reservation, leave the
// model retryable and leave the runtime fully usable. FakeTextEngine = plumbing only, not inference evidence.
#include <gtest/gtest.h>

#include <atomic>

#include "api_fixture.hpp"

using namespace hcs;
using namespace hcs::api;
using namespace std::chrono_literals;

namespace {

constexpr std::uint64_t kPlenty = 1ull << 40;

struct OomRig : test::ApiFixture {
    std::vector<std::string> ids;
    OomRig() {
        budget.update_budget(kPlenty);  // budget known => the reserve path runs
        for (const auto& m : registry.list()) ids.push_back(m.id);
    }
    ModelState state_of(const std::string& id) const { return registry.state(id).value(); }
    ChatParams chat(const std::string& id) const {
        ChatParams p;
        p.model = id;
        p.messages.push_back({"user", "hi", {}});
        return p;
    }
    // Runs one chat to completion; ok() means the job Succeeded, else the job's structured error.
    Result<void> run_chat(const std::string& id) {
        auto sub = cp->submit_chat(chat(id));
        if (!sub)
            return sub.error();
        if (!cp->wait_job(sub.value().id, 10s))
            return make_error(ErrorCode::Internal, "job did not finish");
        auto st = cp->job_status(sub.value().id);
        if (!st)
            return st.error();
        if (st.value().state == JobState::Succeeded)
            return {};
        return st.value().error ? *st.value().error
                                : make_error(ErrorCode::Internal, "job ended without success");
    }
};

}  // namespace

TEST(BudgetFaultSeam, InjectedRefusalIsAnOutOfMemoryAndLeavesTheLedgerUntouched) {
    MemoryBudget b(kPlenty);
    b.set_fault_injector([](const std::string& key, MemoryCategory, std::uint64_t) { return key == "x"; });
    ASSERT_TRUE(b.reserve("y", MemoryCategory::ModelWeights, 100).ok());
    auto r = b.reserve("x", MemoryCategory::KvCache, 50);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::OutOfMemory);
    EXPECT_EQ(b.reserved(), 100u);
    EXPECT_EQ(b.entries().size(), 1u);
    EXPECT_EQ(b.injected_failures(), 1u);
    // A growing resize is refused through the same seam; shrinking is never an allocation.
    b.set_fault_injector([](const std::string&, MemoryCategory, std::uint64_t) { return true; });
    auto g = b.resize("y", 200);
    ASSERT_FALSE(g.ok());
    EXPECT_EQ(g.error().code, ErrorCode::OutOfMemory);
    EXPECT_EQ(b.reserved(), 100u);
    EXPECT_TRUE(b.resize("y", 40).ok());
    EXPECT_EQ(b.reserved(), 40u);
    // Disabling the hook restores normal behaviour.
    b.set_fault_injector({});
    EXPECT_TRUE(b.reserve("x", MemoryCategory::KvCache, 50).ok());
}

TEST(OomInjection, RefusedWeightsReservationRollsBackAndTheModelIsRetryable) {
    OomRig rig;
    ASSERT_GE(rig.ids.size(), 2u);
    const std::string id = rig.ids[0];
    std::atomic<int> fail_left{1};
    rig.budget.set_fault_injector([&](const std::string& key, MemoryCategory, std::uint64_t) {
        return key.rfind("model:", 0) == 0 && fail_left.fetch_sub(1) > 0;
    });
    auto r = rig.cp->load_model(id);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::OutOfMemory);
    EXPECT_EQ(rig.budget.reserved(), 0u) << "a partial reservation leaked";
    EXPECT_EQ(rig.engine.loads.load(), 0) << "the engine must not be touched after a refused allocation";
    EXPECT_FALSE(rig.cp->loaded_model().has_value());
    EXPECT_EQ(rig.state_of(id), ModelState::Failed);
    // The runtime is still usable: the same model loads and answers once memory is available again.
    ASSERT_TRUE(rig.cp->load_model(id).ok());
    EXPECT_EQ(rig.state_of(id), ModelState::Loaded);
    auto text = rig.run_chat(id);
    ASSERT_TRUE(text.ok()) << text.error().message;
}

TEST(OomInjection, RefusedKvReservationReleasesTheAlreadyReservedWeights) {
    OomRig rig;
    const std::string id = rig.ids[0];
    rig.budget.set_fault_injector([](const std::string& key, MemoryCategory c, std::uint64_t) {
        return c == MemoryCategory::KvCache && key.rfind("kv:", 0) == 0;
    });
    auto r = rig.cp->load_model(id);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::OutOfMemory);
    // The weights reservation made first must have been rolled back, not left holding budget.
    EXPECT_EQ(rig.budget.reserved(), 0u);
    EXPECT_TRUE(rig.budget.entries().empty());
    EXPECT_EQ(rig.engine.loads.load(), 0);
    rig.budget.set_fault_injector({});
    EXPECT_TRUE(rig.cp->load_model(id).ok());
    EXPECT_GT(rig.budget.reserved(), 0u);
}

TEST(OomInjection, EngineReturnedOutOfMemoryRollsBackAndKeepsTheCode) {
    OomRig rig;
    const std::string id = rig.ids[0];
    rig.engine.fail_load = true;
    rig.engine.fail_load_code = ErrorCode::OutOfMemory;
    auto r = rig.cp->load_model(id);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::OutOfMemory);
    EXPECT_EQ(rig.budget.reserved(), 0u);
    EXPECT_EQ(rig.state_of(id), ModelState::Failed);
    EXPECT_FALSE(rig.cp->loaded_model().has_value());
    rig.engine.fail_load = false;
    EXPECT_TRUE(rig.cp->load_model(id).ok());
}

// RED before this wave: std::bad_alloc escaping the engine was reported as a generic BackendError.
TEST(OomInjection, BadAllocEscapingTheEngineLoadIsAStructuredOutOfMemory) {
    OomRig rig;
    const std::string id = rig.ids[0];
    rig.engine.throw_bad_alloc_on_load = true;
    auto r = rig.cp->load_model(id);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::OutOfMemory) << r.error().message;
    EXPECT_NE(r.error().message.find("out of memory"), std::string::npos);
    EXPECT_EQ(rig.budget.reserved(), 0u);
    EXPECT_EQ(rig.state_of(id), ModelState::Failed);
    rig.engine.throw_bad_alloc_on_load = false;
    ASSERT_TRUE(rig.cp->load_model(id).ok());
    auto text = rig.run_chat(id);
    EXPECT_TRUE(text.ok());
}

TEST(OomInjection, BadAllocDuringGenerationFailsTheJobAndTheModelStaysLoaded) {
    OomRig rig;
    const std::string id = rig.ids[0];
    ASSERT_TRUE(rig.cp->load_model(id).ok());
    rig.engine.throw_bad_alloc_on_generate = true;
    auto text = rig.run_chat(id);
    ASSERT_FALSE(text.ok());
    EXPECT_EQ(text.error().code, ErrorCode::OutOfMemory) << text.error().message;
    EXPECT_EQ(rig.state_of(id), ModelState::Loaded);  // Running -> Loaded again; not wedged
    rig.engine.throw_bad_alloc_on_generate = false;
    auto again = rig.run_chat(id);
    ASSERT_TRUE(again.ok()) << again.error().message;
}

TEST(OomInjection, RefusedSwitchLeavesNoStaleReservationAndTheNextLoadWorks) {
    OomRig rig;
    ASSERT_GE(rig.ids.size(), 2u);
    ASSERT_TRUE(rig.cp->load_model(rig.ids[0]).ok());
    EXPECT_GT(rig.budget.reserved(), 0u);
    rig.budget.set_fault_injector(
        [&](const std::string& key, MemoryCategory, std::uint64_t) { return key == "model:" + rig.ids[1]; });
    auto r = rig.cp->load_model(rig.ids[1]);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::OutOfMemory);
    // The previous model was evicted to make room (the fit check runs before eviction, the reservation
    // after); nothing of either model may stay in the ledger and no model may be reported loaded.
    EXPECT_EQ(rig.budget.reserved(), 0u);
    EXPECT_FALSE(rig.cp->loaded_model().has_value());
    rig.budget.set_fault_injector({});
    ASSERT_TRUE(rig.cp->load_model(rig.ids[1]).ok());
    EXPECT_EQ(rig.cp->loaded_model().value_or(""), rig.ids[1]);
}

TEST(OomInjection, ManyRefusalsInARowNeverLeakBudget) {
    OomRig rig;
    const std::string id = rig.ids[0];
    rig.budget.set_fault_injector([](const std::string&, MemoryCategory, std::uint64_t) { return true; });
    for (int i = 0; i < 25; ++i) {
        auto r = rig.cp->load_model(id);
        ASSERT_FALSE(r.ok());
        EXPECT_EQ(r.error().code, ErrorCode::OutOfMemory);
        ASSERT_EQ(rig.budget.reserved(), 0u) << "iteration " << i;
    }
    EXPECT_GE(rig.budget.injected_failures(), 25u);
    rig.budget.set_fault_injector({});
    EXPECT_TRUE(rig.cp->load_model(id).ok());
}
