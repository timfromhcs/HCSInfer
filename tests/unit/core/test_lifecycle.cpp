#include <gtest/gtest.h>

#include <atomic>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include "hcs/lifecycle.hpp"

using namespace hcs;
using S = ModelState;

namespace {
const std::vector<S> kAll = {S::Unregistered, S::Discovered, S::Validating, S::ReadyToLoad, S::Loading,
                             S::Loaded,       S::Running,    S::Unloading,  S::Unloaded,    S::Failed};

// Independent statement of the intended table (kept separate from the implementation on purpose).
const std::set<std::pair<S, S>> kLegal = {
    {S::Unregistered, S::Discovered}, {S::Discovered, S::Validating}, {S::Validating, S::ReadyToLoad},
    {S::Validating, S::Failed},       {S::ReadyToLoad, S::Loading},   {S::ReadyToLoad, S::Validating},
    {S::Loading, S::Loaded},          {S::Loading, S::Failed},        {S::Loaded, S::Running},
    {S::Loaded, S::Unloading},        {S::Loaded, S::Failed},         {S::Running, S::Loaded},
    {S::Running, S::Unloading},       {S::Running, S::Failed},        {S::Unloading, S::Unloaded},
    {S::Unloading, S::Failed},        {S::Unloaded, S::Loading},      {S::Unloaded, S::Validating},
    {S::Failed, S::Validating},       {S::Failed, S::Unloading},
};

void drive_to(ModelLifecycle& m, S target) {
    switch (target) {
        case S::Unregistered:
            return;
        case S::Discovered:
            ASSERT_TRUE(m.transition(S::Discovered));
            return;
        case S::Validating:
            drive_to(m, S::Discovered);
            ASSERT_TRUE(m.transition(S::Validating));
            return;
        case S::ReadyToLoad:
            drive_to(m, S::Validating);
            ASSERT_TRUE(m.transition(S::ReadyToLoad));
            return;
        case S::Loading:
            drive_to(m, S::ReadyToLoad);
            ASSERT_TRUE(m.transition(S::Loading));
            return;
        case S::Loaded:
            drive_to(m, S::Loading);
            ASSERT_TRUE(m.transition(S::Loaded));
            return;
        case S::Running:
            drive_to(m, S::Loaded);
            ASSERT_TRUE(m.transition(S::Running));
            return;
        case S::Unloading:
            drive_to(m, S::Loaded);
            ASSERT_TRUE(m.transition(S::Unloading));
            return;
        case S::Unloaded:
            drive_to(m, S::Unloading);
            ASSERT_TRUE(m.transition(S::Unloaded));
            return;
        case S::Failed:
            drive_to(m, S::Loading);
            ASSERT_TRUE(m.fail(FailureKind::Recoverable, "x"));
            return;
    }
}
}  // namespace

TEST(Lifecycle, StaticTableMatchesSpecification) {
    for (S from : kAll) {
        for (S to : kAll) {
            EXPECT_EQ(is_legal_transition(from, to), kLegal.count({from, to}) == 1)
                << to_string(from) << " -> " << to_string(to);
        }
    }
}

TEST(Lifecycle, EveryPairBehavesAccordingToTable) {
    for (S from : kAll) {
        for (S to : kAll) {
            ModelLifecycle m;
            drive_to(m, from);
            ASSERT_EQ(m.state(), from);
            auto r = (to == S::Failed) ? m.fail(FailureKind::Recoverable, "r") : m.transition(to);
            const bool legal = kLegal.count({from, to}) == 1;
            EXPECT_EQ(r.ok(), legal) << to_string(from) << " -> " << to_string(to);
            if (!legal) {
                EXPECT_EQ(m.state(), from) << "state must be unchanged after an illegal transition";
                EXPECT_TRUE(r.error().code == ErrorCode::InvalidState ||
                            r.error().code == ErrorCode::InvalidArgument);
            }
        }
    }
}

TEST(Lifecycle, FullHappyPathAndReload) {
    ModelLifecycle m;
    for (S s : {S::Discovered, S::Validating, S::ReadyToLoad, S::Loading, S::Loaded, S::Running, S::Loaded,
                S::Unloading, S::Unloaded, S::Loading, S::Loaded}) {
        ASSERT_TRUE(m.transition(s)) << to_string(s);
    }
    EXPECT_EQ(m.state(), S::Loaded);
    EXPECT_EQ(m.history().front(), S::Unregistered);
    EXPECT_EQ(m.history().size(), 12u);
}

TEST(Lifecycle, TransitionToFailedRequiresFail) {
    ModelLifecycle m;
    drive_to(m, S::Loading);
    auto r = m.transition(S::Failed);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    EXPECT_FALSE(m.fail(FailureKind::None, "bad"));
    EXPECT_EQ(m.state(), S::Loading);
}

TEST(Lifecycle, RecoverableFailureCanRevalidate) {
    ModelLifecycle m;
    drive_to(m, S::Loading);
    ASSERT_TRUE(m.fail(FailureKind::Recoverable, "oom"));
    EXPECT_EQ(m.failure_kind(), FailureKind::Recoverable);
    EXPECT_EQ(m.failure_reason(), "oom");
    ASSERT_TRUE(m.transition(S::Validating));
    EXPECT_EQ(m.failure_kind(), FailureKind::None);
    EXPECT_TRUE(m.failure_reason().empty());
}

TEST(Lifecycle, NonRecoverableFailureOnlyAllowsUnloading) {
    ModelLifecycle m;
    drive_to(m, S::Loading);
    ASSERT_TRUE(m.fail(FailureKind::NonRecoverable, "device lost"));
    auto r = m.transition(S::Validating);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::InvalidState);
    EXPECT_EQ(m.state(), S::Failed);
    ASSERT_TRUE(m.transition(S::Unloading));
    ASSERT_TRUE(m.transition(S::Unloaded));
}

TEST(Lifecycle, CannotFailFromIdleStates) {
    for (S s : {S::Unregistered, S::Discovered, S::ReadyToLoad, S::Unloaded}) {
        ModelLifecycle m;
        drive_to(m, s);
        EXPECT_FALSE(m.fail(FailureKind::Recoverable, "x")) << to_string(s);
    }
}

TEST(Lifecycle, CompareAndTransition) {
    ModelLifecycle m;
    drive_to(m, S::ReadyToLoad);
    EXPECT_FALSE(m.transition_from(S::Loaded, S::Unloading));
    EXPECT_TRUE(m.transition_from(S::ReadyToLoad, S::Loading));
    EXPECT_EQ(m.state(), S::Loading);
}

TEST(Lifecycle, ExactlyOneConcurrentOwnerWinsTheLoad) {
    for (int round = 0; round < 50; ++round) {
        ModelLifecycle m;
        drive_to(m, S::ReadyToLoad);
        std::atomic<int> winners{0};
        std::vector<std::thread> ts;
        for (int i = 0; i < 8; ++i) {
            ts.emplace_back([&] {
                if (m.transition_from(S::ReadyToLoad, S::Loading))
                    ++winners;
            });
        }
        for (auto& t : ts) t.join();
        EXPECT_EQ(winners.load(), 1);
        EXPECT_EQ(m.state(), S::Loading);
    }
}

TEST(Lifecycle, NamesAreDistinct) {
    std::set<std::string_view> names;
    for (S s : kAll) EXPECT_TRUE(names.insert(to_string(s)).second);
}
