#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>

#include "hcs/job_queue.hpp"

using namespace hcs;
using namespace std::chrono_literals;

namespace {
// Blocks a worker until released, so later jobs stay Queued deterministically.
struct Gate {
    std::promise<void> started_p, release_p;
    std::shared_future<void> release = release_p.get_future().share();
    std::future<void> started = started_p.get_future();
    JobFn fn() {
        return [this](JobContext& ctx) -> Result<void> {
            started_p.set_value();
            while (!ctx.cancelled() && release.wait_for(1ms) != std::future_status::ready) {
            }
            if (ctx.cancelled())
                return make_error(ErrorCode::Cancelled, "cancelled");
            return {};
        };
    }
};
}  // namespace

TEST(JobQueue, RunsJobToSuccessWithEvents) {
    JobQueue q;
    auto id = q.submit("hello", [](JobContext& ctx) -> Result<void> {
        ctx.progress(0.5, "half");
        ctx.output("tok1");
        ctx.output("tok2");
        ctx.log("note");
        return {};
    });
    ASSERT_TRUE(id);
    ASSERT_TRUE(q.wait(id.value(), 5s));
    auto st = q.status(id.value());
    ASSERT_TRUE(st);
    EXPECT_EQ(st.value().state, JobState::Succeeded);
    EXPECT_EQ(st.value().name, "hello");

    auto ev = q.events(id.value());
    ASSERT_GE(ev.size(), 6u);  // queued, running, progress, 2x output, log, succeeded
    for (std::size_t i = 0; i < ev.size(); ++i) EXPECT_EQ(ev[i].seq, i);
    EXPECT_EQ(ev.front().kind, JobEventKind::StateChanged);
    EXPECT_EQ(ev.front().state, JobState::Queued);
    EXPECT_EQ(ev.back().state, JobState::Succeeded);
    std::string streamed;
    for (const auto& e : ev) {
        if (e.kind == JobEventKind::Output)
            streamed += e.text;
    }
    EXPECT_EQ(streamed, "tok1tok2");
    EXPECT_EQ(q.events(id.value(), 3).front().seq, 3u);
}

TEST(JobQueue, FailureCarriesError) {
    JobQueue q;
    auto id = q.submit("bad",
                       [](JobContext&) -> Result<void> { return make_error(ErrorCode::ModelError, "boom"); });
    ASSERT_TRUE(q.wait(id.value(), 5s));
    auto st = q.status(id.value()).value();
    EXPECT_EQ(st.state, JobState::Failed);
    ASSERT_TRUE(st.error.has_value());
    EXPECT_EQ(st.error->code, ErrorCode::ModelError);
}

TEST(JobQueue, ThrowingJobBecomesInternalFailureAndQueueSurvives) {
    JobQueue q;
    auto a = q.submit("throws", [](JobContext&) -> Result<void> { throw std::runtime_error("kaboom"); });
    ASSERT_TRUE(q.wait(a.value(), 5s));
    auto st = q.status(a.value()).value();
    EXPECT_EQ(st.state, JobState::Failed);
    EXPECT_EQ(st.error->code, ErrorCode::Internal);
    auto b = q.submit("after", [](JobContext&) -> Result<void> { return {}; });
    ASSERT_TRUE(q.wait(b.value(), 5s));
    EXPECT_EQ(q.status(b.value()).value().state, JobState::Succeeded);
}

TEST(JobQueue, CancelRunningJobIsCooperative) {
    JobQueue q;
    std::promise<void> started;
    auto id = q.submit("spin", [&](JobContext& ctx) -> Result<void> {
        started.set_value();
        while (!ctx.cancelled()) std::this_thread::sleep_for(1ms);
        return make_error(ErrorCode::Cancelled, "stopped");
    });
    started.get_future().wait();
    EXPECT_EQ(q.status(id.value()).value().state, JobState::Running);
    ASSERT_TRUE(q.cancel(id.value()));
    ASSERT_TRUE(q.wait(id.value(), 5s));
    EXPECT_EQ(q.status(id.value()).value().state, JobState::Cancelled);
}

TEST(JobQueue, CancelQueuedJobNeverRuns) {
    JobQueue q;
    Gate gate;
    auto first = q.submit("gate", gate.fn());
    gate.started.wait();
    std::atomic<bool> ran{false};
    auto second = q.submit("queued", [&](JobContext&) -> Result<void> {
        ran = true;
        return {};
    });
    EXPECT_EQ(q.pending(), 1u);
    EXPECT_EQ(q.status(second.value()).value().state, JobState::Queued);
    ASSERT_TRUE(q.cancel(second.value()));
    EXPECT_EQ(q.status(second.value()).value().state, JobState::Cancelled);
    EXPECT_EQ(q.pending(), 0u);
    gate.release_p.set_value();
    ASSERT_TRUE(q.wait(first.value(), 5s));
    std::this_thread::sleep_for(20ms);
    EXPECT_FALSE(ran.load());
}

TEST(JobQueue, CancelFinishedIsNoOpAndUnknownIsNotFound) {
    JobQueue q;
    auto id = q.submit("x", [](JobContext&) -> Result<void> { return {}; });
    ASSERT_TRUE(q.wait(id.value(), 5s));
    EXPECT_TRUE(q.cancel(id.value()));
    EXPECT_EQ(q.status(id.value()).value().state, JobState::Succeeded);
    EXPECT_EQ(q.cancel(JobId{9999}).error().code, ErrorCode::NotFound);
    EXPECT_EQ(q.status(JobId{9999}).error().code, ErrorCode::NotFound);
    EXPECT_FALSE(q.wait(JobId{9999}, 1ms));
}

TEST(JobQueue, FullQueueRejectsWithBusy) {
    JobQueueOptions o;
    o.max_pending = 2;
    JobQueue q(o);
    Gate gate;
    auto first = q.submit("gate", gate.fn());
    gate.started.wait();
    auto noop = [](JobContext&) -> Result<void> { return {}; };
    ASSERT_TRUE(q.submit("a", noop));
    ASSERT_TRUE(q.submit("b", noop));
    auto c = q.submit("c", noop);
    ASSERT_FALSE(c);
    EXPECT_EQ(c.error().code, ErrorCode::Busy);
    gate.release_p.set_value();
    ASSERT_TRUE(q.wait(first.value(), 5s));
}

TEST(JobQueue, JobsRunInSubmissionOrderOnOneWorker) {
    JobQueue q;
    std::mutex m;
    std::vector<int> order;
    std::vector<JobId> ids;
    for (int i = 0; i < 20; ++i) {
        auto id = q.submit("j", [&, i](JobContext&) -> Result<void> {
            std::lock_guard l(m);
            order.push_back(i);
            return {};
        });
        ASSERT_TRUE(id);
        ids.push_back(id.value());
    }
    for (auto id : ids) ASSERT_TRUE(q.wait(id, 5s));
    ASSERT_EQ(order.size(), 20u);
    for (int i = 0; i < 20; ++i) EXPECT_EQ(order[static_cast<std::size_t>(i)], i);
}

TEST(JobQueue, MultipleWorkersRunConcurrently) {
    JobQueueOptions o;
    o.workers = 4;
    JobQueue q(o);
    std::atomic<int> active{0}, peak{0};
    std::vector<JobId> ids;
    for (int i = 0; i < 8; ++i) {
        ids.push_back(q.submit("c", [&](JobContext&) -> Result<void> {
                           int a = ++active;
                           int p = peak.load();
                           while (a > p && !peak.compare_exchange_weak(p, a)) {
                           }
                           std::this_thread::sleep_for(30ms);
                           --active;
                           return {};
                       }).value());
    }
    for (auto id : ids) ASSERT_TRUE(q.wait(id, 10s));
    EXPECT_GT(peak.load(), 1);
    EXPECT_LE(peak.load(), 4);
}

TEST(JobQueue, EventCallbackReceivesStreamAndMayThrow) {
    JobQueue q;
    std::mutex m;
    std::string text;
    auto id = q.submit(
        "cb",
        [](JobContext& ctx) -> Result<void> {
            ctx.output("a");
            ctx.output("b");
            return {};
        },
        [&](const JobEvent& e) {
            if (e.kind == JobEventKind::Output) {
                std::lock_guard l(m);
                text += e.text;
            }
            throw std::runtime_error("subscriber bug");
        });
    ASSERT_TRUE(q.wait(id.value(), 5s));
    std::lock_guard l(m);
    EXPECT_EQ(text, "ab");
    EXPECT_EQ(q.status(id.value()).value().state, JobState::Succeeded);
}

TEST(JobQueue, ProgressEventsAreBoundedButOutputIsNot) {
    JobQueueOptions o;
    o.max_events_per_job = 10;
    JobQueue q(o);
    auto id = q.submit("flood", [](JobContext& ctx) -> Result<void> {
        for (int i = 0; i < 1000; ++i) ctx.progress(i / 1000.0);
        for (int i = 0; i < 50; ++i) ctx.output("x");
        return {};
    });
    ASSERT_TRUE(q.wait(id.value(), 5s));
    auto ev = q.events(id.value());
    std::size_t outputs = 0, progress = 0;
    for (const auto& e : ev) {
        outputs += e.kind == JobEventKind::Output;
        progress += e.kind == JobEventKind::Progress;
    }
    EXPECT_EQ(outputs, 50u);
    EXPECT_LE(progress, 10u);
}

TEST(JobQueue, ShutdownCancelsRunningAndQueuedAndRejectsNewWork) {
    auto q = std::make_unique<JobQueue>();
    Gate gate;
    auto running = q->submit("run", gate.fn());
    gate.started.wait();
    auto queued = q->submit("queued", [](JobContext&) -> Result<void> { return {}; });
    q->shutdown();
    EXPECT_EQ(q->status(running.value()).value().state, JobState::Cancelled);
    EXPECT_EQ(q->status(queued.value()).value().state, JobState::Cancelled);
    EXPECT_EQ(q->submit("late", [](JobContext&) -> Result<void> { return {}; }).error().code,
              ErrorCode::InvalidState);
    q.reset();  // destructor after shutdown must be safe
}

TEST(JobQueue, DestructorCancelsWithoutHanging) {
    std::promise<void> started;
    {
        JobQueue q;
        (void)q.submit("spin", [&](JobContext& ctx) -> Result<void> {
            started.set_value();
            while (!ctx.cancelled()) std::this_thread::sleep_for(1ms);
            return make_error(ErrorCode::Cancelled, "bye");
        });
        started.get_future().wait();
    }
    SUCCEED();
}

TEST(JobQueue, RetainsBoundedNumberOfFinishedJobs) {
    JobQueueOptions o;
    o.max_retained_jobs = 5;
    JobQueue q(o);
    JobId last;
    for (int i = 0; i < 20; ++i) {
        last = q.submit("j", [](JobContext&) -> Result<void> { return {}; }).value();
        ASSERT_TRUE(q.wait(last, 5s));
    }
    EXPECT_TRUE(q.status(last));
    EXPECT_EQ(q.status(JobId{1}).error().code, ErrorCode::NotFound);  // evicted
}

TEST(JobQueue, RejectsEmptyFunction) {
    JobQueue q;
    EXPECT_EQ(q.submit("x", JobFn{}).error().code, ErrorCode::InvalidArgument);
}

TEST(CancellationToken, SourceAndToken) {
    CancellationSource src;
    auto tok = src.token();
    EXPECT_FALSE(tok.cancelled());
    src.request_cancel();
    EXPECT_TRUE(tok.cancelled());
    EXPECT_TRUE(src.cancelled());
    EXPECT_FALSE(CancellationToken{}.cancelled());  // default token is never cancelled
}
