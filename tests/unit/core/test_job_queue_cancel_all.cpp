// JobQueue::cancel_all / wait_idle: the primitive under the overload kill switch (plan F1).
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "hcs/job_queue.hpp"

using namespace hcs;
using namespace std::chrono_literals;

namespace {
JobFn spin_until_cancelled(std::atomic<int>& started) {
    return [&started](JobContext& ctx) -> Result<void> {
        ++started;
        while (!ctx.cancelled()) std::this_thread::sleep_for(1ms);
        return make_error(ErrorCode::Cancelled, "cancelled");
    };
}
}  // namespace

TEST(JobQueueCancelAll, CancelsRunningAndQueuedJobsAndReachesIdle) {
    JobQueue q;  // one worker: the first job runs, the others stay queued
    std::atomic<int> started{0};
    auto a = q.submit("a", spin_until_cancelled(started));
    auto b = q.submit("b", spin_until_cancelled(started));
    auto c = q.submit("c", spin_until_cancelled(started));
    ASSERT_TRUE(a && b && c);
    while (started.load() < 1) std::this_thread::sleep_for(1ms);
    EXPECT_FALSE(q.wait_idle(20ms));  // busy
    EXPECT_EQ(q.cancel_all(), 3u);
    EXPECT_TRUE(q.wait_idle(5s));
    for (auto id : {a.value(), b.value(), c.value()})
        EXPECT_EQ(q.status(id).value().state, JobState::Cancelled);
    EXPECT_EQ(started.load(), 1);  // the queued ones never started
    EXPECT_EQ(q.pending(), 0u);
}

TEST(JobQueueCancelAll, IsANoOpOnAnIdleQueueAndTheQueueStaysUsable) {
    JobQueue q;
    EXPECT_EQ(q.cancel_all(), 0u);
    EXPECT_TRUE(q.wait_idle(10ms));
    auto id = q.submit("after", [](JobContext&) -> Result<void> { return {}; });
    ASSERT_TRUE(id);
    EXPECT_TRUE(q.wait(id.value(), 5s));
    EXPECT_EQ(q.status(id.value()).value().state, JobState::Succeeded);
}

TEST(JobQueueCancelAll, DoesNotTouchFinishedJobs) {
    JobQueue q;
    auto id = q.submit("done", [](JobContext&) -> Result<void> { return {}; });
    ASSERT_TRUE(q.wait(id.value(), 5s));
    EXPECT_EQ(q.cancel_all(), 0u);
    EXPECT_EQ(q.status(id.value()).value().state, JobState::Succeeded);
}

TEST(JobQueueCancelAll, WaitIdleTimesOutWhileAJobIgnoresItsToken) {
    JobQueue q;
    std::atomic<bool> release{false}, started{false};
    auto id = q.submit("stubborn", [&](JobContext&) -> Result<void> {
        started = true;
        while (!release) std::this_thread::sleep_for(1ms);  // ignores cancellation (an engine load)
        return {};
    });
    ASSERT_TRUE(id);
    while (!started) std::this_thread::sleep_for(1ms);
    EXPECT_EQ(q.cancel_all(), 1u);
    EXPECT_FALSE(q.wait_idle(50ms));  // bounded wait: reports "not idle" instead of hanging
    release = true;
    EXPECT_TRUE(q.wait_idle(5s));
}
