#pragma once
// Bounded job queue with cooperative cancellation and per-job event streams.
// Default 1 worker: engine operations are serialised (AGENTS.md 3.2).
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "hcs/result.hpp"
#include "hcs/types.hpp"

namespace hcs {

class CancellationToken {
public:
    CancellationToken() = default;
    bool cancelled() const noexcept { return flag_ && flag_->load(std::memory_order_acquire); }

private:
    friend class CancellationSource;
    explicit CancellationToken(std::shared_ptr<std::atomic<bool>> f) : flag_(std::move(f)) {}
    std::shared_ptr<std::atomic<bool>> flag_;
};

class CancellationSource {
public:
    CancellationSource() : flag_(std::make_shared<std::atomic<bool>>(false)) {}
    CancellationToken token() const { return CancellationToken(flag_); }
    void request_cancel() noexcept { flag_->store(true, std::memory_order_release); }
    bool cancelled() const noexcept { return flag_->load(std::memory_order_acquire); }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

enum class JobState { Queued, Running, Succeeded, Failed, Cancelled };
enum class JobEventKind { StateChanged, Progress, Output, Log };

std::string_view to_string(JobState s) noexcept;
constexpr bool is_terminal(JobState s) noexcept {
    return s == JobState::Succeeded || s == JobState::Failed || s == JobState::Cancelled;
}

struct JobEvent {
    JobId job;
    std::uint64_t seq = 0;  // per-job, starts at 0
    JobEventKind kind = JobEventKind::Log;
    JobState state = JobState::Queued;  // valid for StateChanged
    double progress = 0.0;              // 0..1, valid for Progress
    std::string text;                   // Output (stream chunk) / Log / Progress message
};

struct JobStatus {
    JobId id;
    std::string name;
    JobState state = JobState::Queued;
    std::optional<Error> error;  // set when Failed (or Cancelled with reason)
    std::size_t event_count = 0;
};

using EventCallback = std::function<void(const JobEvent&)>;

// Handed to the job body. Safe to use only from the job's own thread.
class JobContext {
public:
    JobContext(JobId id, CancellationToken token, std::function<void(JobEventKind, double, std::string)> emit)
        : id_(id), token_(std::move(token)), emit_(std::move(emit)) {}
    JobId id() const noexcept { return id_; }
    const CancellationToken& token() const noexcept { return token_; }
    bool cancelled() const noexcept { return token_.cancelled(); }
    void progress(double fraction, std::string message = {}) {
        emit_(JobEventKind::Progress, fraction, std::move(message));
    }
    void output(std::string chunk) { emit_(JobEventKind::Output, 0.0, std::move(chunk)); }
    void log(std::string message) { emit_(JobEventKind::Log, 0.0, std::move(message)); }

private:
    JobId id_;
    CancellationToken token_;
    std::function<void(JobEventKind, double, std::string)> emit_;
};

using JobFn = std::function<Result<void>(JobContext&)>;

struct JobQueueOptions {
    std::size_t workers = 1;
    std::size_t max_pending = 64;           // queued (not yet running) jobs; submit => Busy beyond
    std::size_t max_events_per_job = 4096;  // Progress/Log dropped beyond; Output/State kept
    std::size_t max_retained_jobs = 256;    // oldest finished jobs are evicted
};

class JobQueue {
public:
    explicit JobQueue(JobQueueOptions options = {});
    ~JobQueue();  // cancels everything and joins workers
    JobQueue(const JobQueue&) = delete;
    JobQueue& operator=(const JobQueue&) = delete;

    Result<JobId> submit(std::string name, JobFn fn, EventCallback on_event = {});
    Result<JobStatus> status(JobId id) const;
    // Queued job: cancelled immediately. Running: token set; job should return ErrorCode::Cancelled.
    // Finished job: no-op success. Unknown id: NotFound.
    Result<void> cancel(JobId id);
    // Kill switch: cancels every queued job at once and signals every running job. Returns how many jobs were
    // affected (queued + running). Running jobs still have to observe their token and return; use wait_idle()
    // to bound that.
    std::size_t cancel_all();
    // Blocks until no job is Queued or Running, or the timeout passes; true iff the queue is idle.
    bool wait_idle(std::chrono::milliseconds timeout) const;
    // Blocks until terminal or timeout; true iff terminal. Unknown id => false.
    bool wait(JobId id, std::chrono::milliseconds timeout) const;
    std::vector<JobEvent> events(JobId id, std::size_t from_seq = 0) const;
    std::size_t pending() const;
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace hcs
