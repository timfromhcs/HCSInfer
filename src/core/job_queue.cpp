#include "hcs/job_queue.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <thread>

namespace hcs {

std::string_view to_string(JobState s) noexcept {
    switch (s) {
        case JobState::Queued:
            return "queued";
        case JobState::Running:
            return "running";
        case JobState::Succeeded:
            return "succeeded";
        case JobState::Failed:
            return "failed";
        case JobState::Cancelled:
            return "cancelled";
    }
    return "unknown";
}

namespace {
struct Job {
    JobId id;
    std::string name;
    JobFn fn;
    EventCallback callback;
    CancellationSource cancel;
    JobState state = JobState::Queued;
    std::optional<Error> error;
    std::vector<JobEvent> events;
    std::uint64_t next_seq = 0;
};
}  // namespace

struct JobQueue::Impl {
    JobQueueOptions options;
    mutable std::mutex mutex;
    mutable std::condition_variable work_cv;  // workers wait here
    mutable std::condition_variable done_cv;  // wait() callers
    std::deque<std::uint64_t> pending;
    std::map<std::uint64_t, std::shared_ptr<Job>> jobs;
    std::uint64_t next_id = 1;
    bool stopping = false;
    std::vector<std::thread> workers;

    // Caller must hold `lock`; releases it around the user callback.
    void emit_locked(std::unique_lock<std::mutex>& lock, Job& job, JobEventKind kind, double progress,
                     std::string text) {
        const bool droppable = kind == JobEventKind::Progress || kind == JobEventKind::Log;
        if (droppable && job.events.size() >= options.max_events_per_job)
            return;
        JobEvent ev;
        ev.job = job.id;
        ev.seq = job.next_seq++;
        ev.kind = kind;
        ev.state = job.state;
        ev.progress = std::clamp(progress, 0.0, 1.0);
        ev.text = std::move(text);
        job.events.push_back(ev);
        if (job.callback) {
            EventCallback cb = job.callback;
            lock.unlock();
            try {
                cb(ev);
            } catch (...) {
                // a throwing subscriber must never break the queue
            }
            lock.lock();
        }
    }

    void evict_finished_locked() {
        while (jobs.size() > options.max_retained_jobs) {
            auto it = std::find_if(jobs.begin(), jobs.end(),
                                   [](const auto& kv) { return is_terminal(kv.second->state); });
            if (it == jobs.end())
                break;
            jobs.erase(it);  // std::map ordered by id => oldest first
        }
    }

    void worker_loop() {
        std::unique_lock lock(mutex);
        for (;;) {
            work_cv.wait(lock, [&] { return stopping || !pending.empty(); });
            if (stopping && pending.empty())
                return;
            const std::uint64_t id = pending.front();
            pending.pop_front();
            auto it = jobs.find(id);
            if (it == jobs.end())
                continue;
            std::shared_ptr<Job> job = it->second;
            if (job->state != JobState::Queued)
                continue;  // cancelled while queued
            job->state = JobState::Running;
            emit_locked(lock, *job, JobEventKind::StateChanged, 0.0, {});

            JobContext ctx(job->id, job->cancel.token(),
                           [this, job](JobEventKind k, double p, std::string t) {
                               std::unique_lock l(mutex);
                               emit_locked(l, *job, k, p, std::move(t));
                           });
            lock.unlock();
            Result<void> result;
            try {
                result = job->fn(ctx);
            } catch (const std::exception& e) {
                result = make_error(ErrorCode::Internal, std::string("job threw: ") + e.what());
            } catch (...) {
                result = make_error(ErrorCode::Internal, "job threw a non-standard exception");
            }
            lock.lock();
            if (result.ok()) {
                job->state = JobState::Succeeded;
            } else if (result.error().code == ErrorCode::Cancelled) {
                job->state = JobState::Cancelled;
                job->error = result.error();
            } else {
                job->state = JobState::Failed;
                job->error = result.error();
            }
            emit_locked(lock, *job, JobEventKind::StateChanged, 1.0,
                        job->error ? job->error->message : std::string());
            job->fn = nullptr;  // release captured resources promptly
            done_cv.notify_all();
            evict_finished_locked();
        }
    }
};

JobQueue::JobQueue(JobQueueOptions options) : impl_(std::make_unique<Impl>()) {
    impl_->options = options;
    if (impl_->options.workers == 0)
        impl_->options.workers = 1;
    for (std::size_t i = 0; i < impl_->options.workers; ++i) {
        impl_->workers.emplace_back([this] { impl_->worker_loop(); });
    }
}

JobQueue::~JobQueue() {
    shutdown();
}

void JobQueue::shutdown() {
    std::vector<std::thread> to_join;
    {
        std::unique_lock lock(impl_->mutex);
        if (impl_->stopping && impl_->workers.empty())
            return;
        impl_->stopping = true;
        // cancel everything queued and running
        while (!impl_->pending.empty()) {
            const auto id = impl_->pending.front();
            impl_->pending.pop_front();
            auto it = impl_->jobs.find(id);
            if (it != impl_->jobs.end() && it->second->state == JobState::Queued) {
                auto& job = *it->second;
                job.state = JobState::Cancelled;
                job.error = Error(ErrorCode::Cancelled, "queue shut down");
                impl_->emit_locked(lock, job, JobEventKind::StateChanged, 0.0, "queue shut down");
            }
        }
        for (auto& kv : impl_->jobs) {
            if (kv.second->state == JobState::Running)
                kv.second->cancel.request_cancel();
        }
        to_join.swap(impl_->workers);
    }
    impl_->work_cv.notify_all();
    for (auto& t : to_join) {
        if (t.joinable())
            t.join();
    }
    impl_->done_cv.notify_all();
}

Result<JobId> JobQueue::submit(std::string name, JobFn fn, EventCallback on_event) {
    if (!fn)
        return make_error(ErrorCode::InvalidArgument, "empty job function");
    std::unique_lock lock(impl_->mutex);
    if (impl_->stopping)
        return make_error(ErrorCode::InvalidState, "job queue is shut down");
    if (impl_->pending.size() >= impl_->options.max_pending) {
        return make_error(ErrorCode::Busy, "job queue is full");
    }
    auto job = std::make_shared<Job>();
    job->id = JobId{impl_->next_id++};
    job->name = std::move(name);
    job->fn = std::move(fn);
    job->callback = std::move(on_event);
    impl_->jobs.emplace(job->id.value, job);
    impl_->pending.push_back(job->id.value);
    impl_->emit_locked(lock, *job, JobEventKind::StateChanged, 0.0, {});
    impl_->work_cv.notify_one();
    return job->id;
}

Result<JobStatus> JobQueue::status(JobId id) const {
    std::lock_guard lock(impl_->mutex);
    auto it = impl_->jobs.find(id.value);
    if (it == impl_->jobs.end())
        return make_error(ErrorCode::NotFound, "unknown job " + to_string(id));
    const Job& j = *it->second;
    JobStatus st;
    st.id = j.id;
    st.name = j.name;
    st.state = j.state;
    st.error = j.error;
    st.event_count = j.events.size();
    return st;
}

Result<void> JobQueue::cancel(JobId id) {
    std::unique_lock lock(impl_->mutex);
    auto it = impl_->jobs.find(id.value);
    if (it == impl_->jobs.end())
        return make_error(ErrorCode::NotFound, "unknown job " + to_string(id));
    std::shared_ptr<Job> job = it->second;
    if (is_terminal(job->state))
        return {};
    if (job->state == JobState::Queued) {
        std::erase(impl_->pending, id.value);
        job->state = JobState::Cancelled;
        job->error = Error(ErrorCode::Cancelled, "cancelled before start");
        job->fn = nullptr;
        impl_->emit_locked(lock, *job, JobEventKind::StateChanged, 0.0, "cancelled before start");
        impl_->done_cv.notify_all();
        return {};
    }
    job->cancel.request_cancel();
    return {};
}

bool JobQueue::wait(JobId id, std::chrono::milliseconds timeout) const {
    std::unique_lock lock(impl_->mutex);
    auto it = impl_->jobs.find(id.value);
    if (it == impl_->jobs.end())
        return false;
    std::shared_ptr<Job> job = it->second;
    return impl_->done_cv.wait_for(lock, timeout, [&] { return is_terminal(job->state); });
}

std::vector<JobEvent> JobQueue::events(JobId id, std::size_t from_seq) const {
    std::lock_guard lock(impl_->mutex);
    auto it = impl_->jobs.find(id.value);
    std::vector<JobEvent> out;
    if (it == impl_->jobs.end())
        return out;
    for (const auto& ev : it->second->events) {
        if (ev.seq >= from_seq)
            out.push_back(ev);
    }
    return out;
}

std::size_t JobQueue::pending() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->pending.size();
}

}  // namespace hcs
