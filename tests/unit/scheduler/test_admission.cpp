// Overload controller (plan F1): thresholds, hysteresis, concurrency bound, kill switch, watchdog.
// The probe is an injected fake (fault injection at the measurement boundary); the logic under test is real.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "fake_probe.hpp"
#include "hcs/scheduler/admission.hpp"

using namespace hcs;
using namespace hcs::scheduler;
using hcs::test::FakeProbe;
using hcs::test::kGiB;
using hcs::test::kMiB;

namespace {
AdmissionConfig cfg() {
    AdmissionConfig c;  // defaults: 2.5 GiB soft floor, 1 GiB hard floor, 512 MiB hysteresis, 90 % commit
    c.max_in_flight = 2;
    c.retry_after = std::chrono::seconds(7);
    c.watchdog_interval = std::chrono::milliseconds(10);
    return c;
}
WorkRequest work(std::uint64_t bytes, WorkKind k = WorkKind::Text) {
    WorkRequest w;
    w.kind = k;
    w.what = "m";
    w.predicted_bytes = bytes;
    return w;
}
bool wait_for(const std::function<bool()>& f, int ms = 3000) {
    for (int i = 0; i < ms / 5; ++i) {
        if (f())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return f();
}
}  // namespace

TEST(AdmissionConfig, DefaultsMatchThePlan) {
    AdmissionConfig c;
    EXPECT_EQ(c.min_free_ram_bytes, 2560 * kMiB);  // keep >= 2.5 GB free
    EXPECT_DOUBLE_EQ(c.max_commit_fraction, 0.90);
    EXPECT_TRUE(validate(c).ok());
}

TEST(AdmissionConfig, RejectsInconsistentThresholds) {
    AdmissionConfig c;
    c.hard_floor_bytes = c.min_free_ram_bytes + 1;
    EXPECT_EQ(validate(c).error().code, ErrorCode::InvalidArgument);
    c = {};
    c.max_commit_fraction = 0.0;
    EXPECT_FALSE(validate(c).ok());
    c = {};
    c.hard_commit_fraction = 0.5;  // below the soft fraction
    EXPECT_FALSE(validate(c).ok());
    c = {};
    c.max_in_flight = 0;
    EXPECT_FALSE(validate(c).ok());
}

TEST(Admission, AdmitsWhenPlentyOfRamIsFree) {
    FakeProbe p;
    AdmissionController a(cfg(), p.probe());
    auto t = a.admit(work(512 * kMiB));
    ASSERT_TRUE(t.ok());
    EXPECT_TRUE(static_cast<bool>(t.value()));
    EXPECT_EQ(a.in_flight(), 1u);
    EXPECT_EQ(a.level(), PressureLevel::Normal);
}

TEST(Admission, RefusesWhenPredictedNeedBreaksTheRamFloor) {
    FakeProbe p;
    p.set_free(4 * kGiB);  // 4 GiB free, floor 2.5 GiB: 1.5 GiB headroom
    AdmissionController a(cfg(), p.probe());
    auto first = a.admit(work(1 * kGiB));
    EXPECT_TRUE(first.ok());
    auto r = a.admit(work(2 * kGiB));  // 4 - 2 = 2 GiB < 2.5 GiB
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::OutOfMemory);
    // The message carries remediation, not just a number.
    EXPECT_NE(r.error().message.find("unload"), std::string::npos);
    EXPECT_NE(r.error().message.find("2560 MiB"), std::string::npos);
    EXPECT_EQ(a.in_flight(), 1u);  // a refusal never takes a slot
}

TEST(Admission, ExactBoundaryIsAllowed) {
    FakeProbe p;
    p.set_free(2560 * kMiB + 100 * kMiB);
    AdmissionController a(cfg(), p.probe());
    EXPECT_TRUE(a.admit(work(100 * kMiB)).ok());                                // leaves exactly the floor
    EXPECT_EQ(a.admit(work(101 * kMiB)).error().code, ErrorCode::OutOfMemory);  // one MiB short
}

TEST(Admission, CommitLimitIsEnforcedIncludingThePredictedNeed) {
    FakeProbe p;
    p.set_commit(80 * kGiB / 10, 10 * kGiB);  // 80 %
    AdmissionController a(cfg(), p.probe());
    EXPECT_TRUE(a.admit(work(512 * kMiB)).ok());  // 85 %
    auto r = a.admit(work(2 * kGiB));             // 100 %
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::OutOfMemory);
    EXPECT_NE(r.error().message.find("commit"), std::string::npos);
}

TEST(Admission, GpuHeapBudgetIsCheckedOnlyWhenKnownAndRequested) {
    FakeProbe p;
    AdmissionController a(cfg(), p.probe());
    WorkRequest w = work(256 * kMiB, WorkKind::Image);
    w.predicted_gpu_bytes = 3 * kGiB;
    p.set_gpu(std::nullopt);  // unknown => the rule is skipped, not guessed
    EXPECT_TRUE(a.admit(w).ok());
    p.set_gpu(1 * kGiB);
    auto r = a.admit(w);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::OutOfMemory);
    EXPECT_NE(r.error().message.find("GPU"), std::string::npos);
    w.predicted_gpu_bytes = 0;  // CPU job: the heap is irrelevant
    EXPECT_TRUE(a.admit(w).ok());
}

TEST(Admission, UnknownMeasurementsDisableOnlyTheRulesThatNeedThem) {
    FakeProbe p;
    p.make_unknown();
    AdmissionController a(cfg(), p.probe());
    EXPECT_TRUE(a.admit(work(64 * kGiB)).ok());  // nothing known: nothing to refuse on (documented)
    EXPECT_EQ(a.level(), PressureLevel::Normal);
    const auto j = a.snapshot_json();
    EXPECT_EQ(j["sample"]["free_physical_bytes"], "unknown");
    EXPECT_EQ(j["sample"]["commit_fraction"], "unknown");
}

TEST(Admission, ProbeThatThrowsMeansUnknownNotACrash) {
    AdmissionController a(cfg(), []() -> telemetry::ResourceSample { throw std::runtime_error("boom"); });
    EXPECT_TRUE(a.admit(work(1 * kMiB)).ok());
    EXPECT_EQ(a.snapshot_json()["sample"]["free_physical_bytes"], "unknown");
}

TEST(Admission, ConcurrencyBoundAndSlotRelease) {
    FakeProbe p;
    AdmissionController a(cfg(), p.probe());  // max_in_flight = 2
    auto t1 = a.admit(work(1 * kMiB));
    auto t2 = a.admit(work(1 * kMiB));
    ASSERT_TRUE(t1.ok());
    ASSERT_TRUE(t2.ok());
    auto t3 = a.admit(work(1 * kMiB));
    ASSERT_FALSE(t3.ok());
    EXPECT_EQ(t3.error().code, ErrorCode::Overloaded);
    EXPECT_NE(t3.error().message.find("retry in 7 s"), std::string::npos);
    EXPECT_EQ(a.retry_after_seconds(), 7);
    {
        Ticket gone = std::move(t1.value());  // dropping the ticket frees the slot
    }
    EXPECT_EQ(a.in_flight(), 1u);
    EXPECT_TRUE(a.admit(work(1 * kMiB)).ok());
}

TEST(Admission, TicketReleaseIsIdempotentAndMovable) {
    FakeProbe p;
    AdmissionController a(cfg(), p.probe());
    auto t = a.admit(work(1 * kMiB));
    ASSERT_TRUE(t.ok());
    Ticket moved = std::move(t.value());
    EXPECT_EQ(a.in_flight(), 1u);
    moved.release();
    moved.release();
    EXPECT_EQ(a.in_flight(), 0u);
}

TEST(Admission, TicketMayOutliveTheController) {
    FakeProbe p;
    Ticket t;
    {
        AdmissionController a(cfg(), p.probe());
        auto r = a.admit(work(1 * kMiB));
        ASSERT_TRUE(r.ok());
        t = std::move(r.value());
    }
    t.release();  // must not touch freed memory (shared state)
    SUCCEED();
}

TEST(Admission, PressureLatchesWithHysteresis) {
    FakeProbe p;
    AdmissionController a(cfg(), p.probe());
    p.set_free(2000 * kMiB);  // below the 2560 MiB soft floor
    auto r = a.admit(work(0));
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Overloaded);
    EXPECT_EQ(a.level(), PressureLevel::Pressure);
    // 3000 MiB is above the floor but inside the hysteresis band (needs 2560 + 512 = 3072): still refused.
    p.set_free(3000 * kMiB);
    EXPECT_EQ(a.admit(work(0)).error().code, ErrorCode::Overloaded);
    EXPECT_EQ(a.level(), PressureLevel::Pressure);
    // Out of the band: admitted again.
    p.set_free(3100 * kMiB);
    EXPECT_TRUE(a.admit(work(0)).ok());
    EXPECT_EQ(a.level(), PressureLevel::Normal);
}

TEST(Admission, ReadingFlappingAroundTheThresholdDoesNotFlap) {
    FakeProbe p;
    AdmissionController a(cfg(), p.probe());
    p.set_free(2400 * kMiB);
    (void)a.admit(work(0));
    int admitted = 0;
    for (int i = 0; i < 20; ++i) {  // oscillate +-100 MiB around the 2560 MiB floor
        p.set_free(i % 2 ? 2480 * kMiB : 2660 * kMiB);
        if (a.admit(work(0)).ok())
            ++admitted;
    }
    EXPECT_EQ(admitted, 0);  // never leaves Pressure inside the band
}

TEST(Admission, CommitPressureAlsoLatches) {
    FakeProbe p;
    p.set_commit(95 * kGiB / 10, 10 * kGiB);  // 95 %: above soft 90 %, below hard 97 %
    AdmissionController a(cfg(), p.probe());
    EXPECT_EQ(a.admit(work(0)).error().code, ErrorCode::Overloaded);
    EXPECT_EQ(a.level(), PressureLevel::Pressure);
    p.set_commit(89 * kGiB / 10, 10 * kGiB);  // 89 %: inside the 2 % band
    EXPECT_FALSE(a.admit(work(0)).ok());
    p.set_commit(80 * kGiB / 10, 10 * kGiB);
    EXPECT_TRUE(a.admit(work(0)).ok());
}

TEST(Admission, CriticalRefusesEverythingIncludingLoadsUntilItRecovers) {
    FakeProbe p;
    AdmissionController a(cfg(), p.probe());
    p.set_free(500 * kMiB);  // below the 1 GiB hard floor
    EXPECT_EQ(a.admit(work(0)).error().code, ErrorCode::Overloaded);
    EXPECT_EQ(a.level(), PressureLevel::Critical);
    EXPECT_EQ(a.check_memory(work(0, WorkKind::ModelLoad)).error().code, ErrorCode::Overloaded);
    p.set_free(1300 * kMiB);  // above the hard floor but inside its band (needs 1536 MiB)
    EXPECT_EQ(a.level(), PressureLevel::Critical);
    EXPECT_FALSE(a.admit(work(0)).ok());
    p.set_free(1600 * kMiB);  // leaves Critical, still below the soft floor
    EXPECT_FALSE(a.admit(work(0)).ok());
    EXPECT_EQ(a.level(), PressureLevel::Pressure);
    p.set_free(8 * kGiB);
    EXPECT_TRUE(a.admit(work(0)).ok());
    EXPECT_EQ(a.level(), PressureLevel::Normal);
}

TEST(Admission, CheckMemoryDoesNotTakeASlot) {
    FakeProbe p;
    AdmissionController a(cfg(), p.probe());
    EXPECT_TRUE(a.check_memory(work(1 * kGiB, WorkKind::ModelLoad)).ok());
    EXPECT_EQ(a.in_flight(), 0u);
    p.set_free(3 * kGiB);
    EXPECT_EQ(a.check_memory(work(1 * kGiB, WorkKind::ModelLoad)).error().code, ErrorCode::OutOfMemory);
    // A load that nets out to zero (the old model is credited) only fails when Critical.
    EXPECT_TRUE(a.check_memory(work(0, WorkKind::ModelLoad)).ok());
}

TEST(Admission, DisabledControllerAdmitsEverythingAndSamplesNothing) {
    FakeProbe p;
    p.set_free(1 * kMiB);
    AdmissionConfig c = cfg();
    c.enabled = false;
    AdmissionController a(c, p.probe());
    EXPECT_TRUE(a.admit(work(64 * kGiB)).ok());
    EXPECT_TRUE(a.check_memory(work(64 * kGiB)).ok());
    EXPECT_EQ(a.poll(), PressureLevel::Normal);
    EXPECT_EQ(p.calls.load(), 0);
    EXPECT_EQ(a.snapshot_json()["enabled"], false);
}

TEST(KillSwitch, ManualShedRunsTheHandlerAndIsRecorded) {
    FakeProbe p;
    AdmissionController a(cfg(), p.probe());
    int calls = 0;
    a.set_shed_handler([&](const std::string& reason) {
        ++calls;
        ShedReport r;
        r.reason = reason;
        r.jobs_cancelled = 3;
        r.model_unloaded = true;
        return r;
    });
    const auto rep = a.shed("because");
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(rep.jobs_cancelled, 3u);
    EXPECT_TRUE(rep.model_unloaded);
    const auto j = a.snapshot_json();
    EXPECT_EQ(j["counters"]["shed_count"], 1);
    EXPECT_EQ(j["last_shed"]["reason"], "because");
    EXPECT_EQ(j["last_shed"]["jobs_cancelled"], 3);
}

TEST(KillSwitch, ShedWithoutHandlerIsASafeNoOp) {
    FakeProbe p;
    AdmissionController a(cfg(), p.probe());
    const auto rep = a.shed("none");
    EXPECT_EQ(rep.jobs_cancelled, 0u);
    EXPECT_FALSE(rep.model_unloaded);
}

TEST(KillSwitch, AThrowingHandlerCannotBreakTheController) {
    FakeProbe p;
    AdmissionController a(cfg(), p.probe());
    a.set_shed_handler([](const std::string&) -> ShedReport { throw std::runtime_error("kaboom"); });
    const auto rep = a.shed("x");
    EXPECT_NE(rep.reason.find("kaboom"), std::string::npos);
    EXPECT_TRUE(a.admit(work(1 * kMiB)).ok());
}

TEST(Watchdog, PollShedsExactlyOncePerCriticalEpisode) {
    FakeProbe p;
    AdmissionController a(cfg(), p.probe());
    std::atomic<int> sheds{0};
    a.set_shed_handler([&](const std::string&) {
        ++sheds;
        return ShedReport{};
    });
    EXPECT_EQ(a.poll(), PressureLevel::Normal);
    EXPECT_EQ(sheds.load(), 0);
    p.set_free(300 * kMiB);
    EXPECT_EQ(a.poll(), PressureLevel::Critical);
    EXPECT_EQ(a.poll(), PressureLevel::Critical);
    EXPECT_EQ(a.poll(), PressureLevel::Critical);
    EXPECT_EQ(sheds.load(), 1);  // not once per poll
    const auto j = a.snapshot_json();
    EXPECT_NE(j["last_shed"]["reason"].get<std::string>().find("hard floor"), std::string::npos);
    // Recover, then a second episode sheds again.
    p.set_free(8 * kGiB);
    EXPECT_EQ(a.poll(), PressureLevel::Normal);
    p.set_free(200 * kMiB);
    (void)a.poll();
    EXPECT_EQ(sheds.load(), 2);
}

TEST(Watchdog, CriticalEnteredThroughAdmitStillShedsOnTheNextPoll) {
    FakeProbe p;
    AdmissionController a(cfg(), p.probe());
    std::atomic<int> sheds{0};
    a.set_shed_handler([&](const std::string&) {
        ++sheds;
        return ShedReport{};
    });
    p.set_free(300 * kMiB);
    (void)a.admit(work(0));  // consumes the transition
    EXPECT_EQ(sheds.load(), 0);
    (void)a.poll();
    EXPECT_EQ(sheds.load(), 1);
}

TEST(Watchdog, CommitAboveTheHardLimitTripsToo) {
    FakeProbe p;
    p.set_commit(99 * kGiB / 10, 10 * kGiB);  // 99 %: above the hard 97 %
    AdmissionController a(cfg(), p.probe());
    std::string why;
    a.set_shed_handler([&](const std::string& r) {
        why = r;
        return ShedReport{};
    });
    EXPECT_EQ(a.poll(), PressureLevel::Critical);
    EXPECT_NE(why.find("commit"), std::string::npos);
}

TEST(Watchdog, BackgroundThreadTripsTheKillSwitchOnItsOwn) {
    FakeProbe p;
    AdmissionController a(cfg(), p.probe());
    std::atomic<int> sheds{0};
    a.set_shed_handler([&](const std::string&) {
        ++sheds;
        return ShedReport{};
    });
    a.start_watchdog();
    a.start_watchdog();  // idempotent
    EXPECT_TRUE(a.snapshot_json()["watchdog_running"].get<bool>());
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT_EQ(sheds.load(), 0);
    p.set_free(100 * kMiB);
    EXPECT_TRUE(wait_for([&] { return sheds.load() == 1; }));
    a.stop_watchdog();
    a.stop_watchdog();  // idempotent
    p.set_free(8 * kGiB);
    p.set_free(100 * kMiB);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT_EQ(sheds.load(), 1);  // stopped: no further sampling
}

TEST(Watchdog, ClearHandlerWaitsForARunningShed) {
    FakeProbe p;
    AdmissionController a(cfg(), p.probe());
    std::atomic<bool> in_handler{false}, done{false};
    a.set_shed_handler([&](const std::string&) {
        in_handler = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        done = true;
        return ShedReport{};
    });
    std::thread t([&] { (void)a.shed("slow"); });
    ASSERT_TRUE(wait_for([&] { return in_handler.load(); }));
    a.clear_shed_handler();
    EXPECT_TRUE(done.load());  // returned only after the handler finished
    t.join();
}

TEST(Estimates, AreMonotonicSaturatingAndAboveTheirFloor) {
    EXPECT_GE(estimate_text_job_bytes(0, 0), 64 * kMiB);
    EXPECT_LT(estimate_text_job_bytes(1000, 100), estimate_text_job_bytes(100000, 100));
    EXPECT_LT(estimate_text_job_bytes(1000, 100), estimate_text_job_bytes(1000, 4096));
    EXPECT_GE(estimate_text_job_bytes(SIZE_MAX, UINT32_MAX), 64 * kMiB);  // no wrap-around to a tiny number
    EXPECT_GE(estimate_vision_job_bytes(0), 128 * kMiB);
    EXPECT_LE(estimate_vision_job_bytes(SIZE_MAX), 128 * kMiB + 768 * kMiB);
    EXPECT_LT(estimate_image_job_bytes(512, 512, false), estimate_image_job_bytes(1024, 1024, false));
    EXPECT_LT(estimate_image_job_bytes(512, 512, false), estimate_image_job_bytes(512, 512, true));
    // A 1 MP SD1.5 job is estimated in the GiB range, not in kilobytes.
    EXPECT_GT(estimate_image_job_bytes(1024, 1024, false), 4 * kGiB);
    EXPECT_GE(estimate_image_job_bytes(UINT32_MAX, UINT32_MAX, true), 256 * kMiB);
}

TEST(WorkNames, AreStable) {
    EXPECT_EQ(to_string(WorkKind::ModelLoad), "model_load");
    EXPECT_EQ(to_string(PressureLevel::Critical), "critical");
    EXPECT_EQ(to_string(ErrorCode::Overloaded), "overloaded");
}
