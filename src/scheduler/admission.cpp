#include "hcs/scheduler/admission.hpp"

#include <algorithm>
#include <condition_variable>
#include <mutex>

#include "hcs/checked_math.hpp"

namespace hcs::scheduler {

namespace {
constexpr std::uint64_t kMiB = 1ull << 20;
constexpr std::uint64_t kKiB = 1ull << 10;

std::string mib(std::uint64_t bytes) {
    return std::to_string(bytes / kMiB) + " MiB";
}

constexpr const char* kRemedy =
    "unload other models, reduce the context size or image resolution, close other applications, or use CPU "
    "mode";
}  // namespace

std::string_view to_string(WorkKind k) noexcept {
    switch (k) {
        case WorkKind::ModelLoad:
            return "model_load";
        case WorkKind::Text:
            return "text";
        case WorkKind::Vision:
            return "vision";
        case WorkKind::Image:
            return "image";
        case WorkKind::Benchmark:
            return "benchmark";
    }
    return "unknown";
}

std::string_view to_string(PressureLevel l) noexcept {
    switch (l) {
        case PressureLevel::Normal:
            return "normal";
        case PressureLevel::Pressure:
            return "pressure";
        case PressureLevel::Critical:
            return "critical";
    }
    return "unknown";
}

Result<void> validate(const AdmissionConfig& c) {
    auto bad = [](const std::string& m) { return Result<void>(make_error(ErrorCode::InvalidArgument, m)); };
    if (c.hard_floor_bytes > c.min_free_ram_bytes)
        return bad("hard RAM floor (" + mib(c.hard_floor_bytes) + ") must not exceed the soft floor (" +
                   mib(c.min_free_ram_bytes) + ")");
    if (!(c.max_commit_fraction > 0.0 && c.max_commit_fraction <= 1.0) ||
        !(c.hard_commit_fraction > 0.0 && c.hard_commit_fraction <= 1.0))
        return bad("commit fractions must be in (0, 1]");
    if (c.hard_commit_fraction < c.max_commit_fraction)
        return bad("hard commit fraction must not be below the soft commit fraction");
    if (!(c.hysteresis_commit_fraction >= 0.0 && c.hysteresis_commit_fraction < 1.0))
        return bad("commit hysteresis must be in [0, 1)");
    if (c.max_in_flight == 0)
        return bad("max_in_flight must be at least 1");
    if (c.retry_after.count() < 0 || c.watchdog_interval.count() <= 0 || c.shed_deadline.count() <= 0)
        return bad("retry_after must be >= 0 and the watchdog interval / shed deadline must be positive");
    return {};
}

nlohmann::json to_json(const ShedReport& r) {
    return {{"reason", r.reason},
            {"jobs_cancelled", r.jobs_cancelled},
            {"model_unloaded", r.model_unloaded},
            {"unload_pending", r.unload_pending},
            {"jobs_idle", r.jobs_idle},
            {"duration_ms", r.duration_ms}};
}

namespace detail {
struct State {
    AdmissionConfig cfg;
    telemetry::ResourceProbe probe;

    std::mutex m;  // guards everything below up to handler_m
    PressureLevel level = PressureLevel::Normal;
    bool shed_pending = false;  // Critical was entered and the watchdog has not shed yet
    std::size_t in_flight = 0;
    std::uint64_t admitted = 0, rejected_memory = 0, rejected_overloaded = 0, shed_count = 0;
    nlohmann::json last_shed = nullptr;
    telemetry::ResourceSample last_sample;
    bool watchdog_running = false;

    std::mutex handler_m;  // serialises set/clear/invocation of the shed handler
    ShedHandler handler;

    std::mutex wd_m;
    std::condition_variable wd_cv;
    bool wd_stop = false;

    telemetry::ResourceSample sample() {
        telemetry::ResourceSample s;
        try {
            s = probe ? probe() : telemetry::ResourceSample{};
        } catch (...) {
            s = telemetry::ResourceSample{};  // a failing probe means "unknown", never a guess
        }
        return s;
    }

    // Updates `level` with hysteresis. Caller holds m. Sets shed_pending on entering Critical.
    void evaluate_locked(const telemetry::ResourceSample& s) {
        last_sample = s;
        const auto frac = s.commit_fraction();
        const bool crit = (s.free_physical_bytes && *s.free_physical_bytes < cfg.hard_floor_bytes) ||
                          (frac && *frac > cfg.hard_commit_fraction);
        const bool press = (s.free_physical_bytes && *s.free_physical_bytes < cfg.min_free_ram_bytes) ||
                           (frac && *frac > cfg.max_commit_fraction);
        const bool crit_clear = (!s.free_physical_bytes ||
                                 *s.free_physical_bytes >= cfg.hard_floor_bytes + cfg.hysteresis_bytes) &&
                                (!frac || *frac <= cfg.hard_commit_fraction - cfg.hysteresis_commit_fraction);
        const bool press_clear = (!s.free_physical_bytes ||
                                  *s.free_physical_bytes >= cfg.min_free_ram_bytes + cfg.hysteresis_bytes) &&
                                 (!frac || *frac <= cfg.max_commit_fraction - cfg.hysteresis_commit_fraction);

        PressureLevel next = level;
        if (level == PressureLevel::Critical) {
            if (crit_clear)
                next = press_clear ? PressureLevel::Normal : PressureLevel::Pressure;
        } else if (crit) {
            next = PressureLevel::Critical;
        } else if (level == PressureLevel::Pressure) {
            if (press_clear)
                next = PressureLevel::Normal;
        } else if (press) {
            next = PressureLevel::Pressure;
        }
        if (next == PressureLevel::Critical && level != PressureLevel::Critical)
            shed_pending = true;
        level = next;
    }

    // Would `r` break the floors? Pure over the sample + config.
    Result<void> fit(const telemetry::ResourceSample& s, const WorkRequest& r) const {
        const std::string what = std::string(to_string(r.kind)) + (r.what.empty() ? "" : " '" + r.what + "'");
        if (s.free_physical_bytes && r.predicted_bytes != 0) {
            const std::uint64_t need =
                checked_add(r.predicted_bytes, cfg.min_free_ram_bytes).value_or(UINT64_MAX);
            if (*s.free_physical_bytes < need)
                return make_error(ErrorCode::OutOfMemory,
                                  what + " needs about " + mib(r.predicted_bytes) + " but only " +
                                      mib(*s.free_physical_bytes) + " of RAM is free and " +
                                      mib(cfg.min_free_ram_bytes) + " must stay free: " + kRemedy);
        }
        if (s.commit_used_bytes && s.commit_limit_bytes && *s.commit_limit_bytes != 0 &&
            r.predicted_bytes != 0) {
            const std::uint64_t after =
                checked_add(*s.commit_used_bytes, r.predicted_bytes).value_or(UINT64_MAX);
            const double frac = static_cast<double>(after) / static_cast<double>(*s.commit_limit_bytes);
            if (frac > cfg.max_commit_fraction)
                return make_error(ErrorCode::OutOfMemory,
                                  what + " would raise the commit charge to " +
                                      std::to_string(static_cast<int>(frac * 100.0)) + "% (limit " +
                                      std::to_string(static_cast<int>(cfg.max_commit_fraction * 100.0)) +
                                      "%): " + kRemedy);
        }
        if (cfg.check_gpu_heap && s.gpu_heap_headroom_bytes && r.predicted_gpu_bytes != 0 &&
            r.predicted_gpu_bytes > *s.gpu_heap_headroom_bytes)
            return make_error(ErrorCode::OutOfMemory, what + " needs about " + mib(r.predicted_gpu_bytes) +
                                                          " of GPU memory but only " +
                                                          mib(*s.gpu_heap_headroom_bytes) +
                                                          " of the GPU heap budget is free: " + kRemedy);
        return {};
    }
};
}  // namespace detail

Ticket& Ticket::operator=(Ticket&& o) noexcept {
    if (this != &o) {
        release();
        state_ = std::move(o.state_);
    }
    return *this;
}

void Ticket::release() noexcept {
    if (!state_)
        return;
    {
        std::lock_guard lk(state_->m);
        if (state_->in_flight > 0)
            --state_->in_flight;
    }
    state_.reset();
}

AdmissionController::AdmissionController(AdmissionConfig config, telemetry::ResourceProbe probe)
    : cfg_(config), st_(std::make_shared<detail::State>()) {
    st_->cfg = cfg_;
    st_->probe = std::move(probe);
}

AdmissionController::~AdmissionController() {
    stop_watchdog();
}

Result<Ticket> AdmissionController::admit(const WorkRequest& req) {
    if (!cfg_.enabled)
        return Ticket{};
    const auto s = st_->sample();
    {
        std::lock_guard lk(st_->m);
        st_->evaluate_locked(s);
        if (st_->level == PressureLevel::Critical) {
            ++st_->rejected_overloaded;
            return make_error(ErrorCode::Overloaded,
                              "runtime is overloaded (free RAM below the hard floor of " +
                                  mib(cfg_.hard_floor_bytes) + "): load is being shed; retry in " +
                                  std::to_string(cfg_.retry_after.count()) + " s");
        }
        if (st_->level == PressureLevel::Pressure) {
            ++st_->rejected_overloaded;
            return make_error(ErrorCode::Overloaded,
                              "runtime is under memory pressure (admits again once at least " +
                                  mib(cfg_.min_free_ram_bytes + cfg_.hysteresis_bytes) +
                                  " of RAM is free); retry in " + std::to_string(cfg_.retry_after.count()) +
                                  " s");
        }
        if (st_->in_flight >= cfg_.max_in_flight) {
            ++st_->rejected_overloaded;
            return make_error(ErrorCode::Overloaded, std::to_string(st_->in_flight) +
                                                         " jobs are already in flight (limit " +
                                                         std::to_string(cfg_.max_in_flight) + "); retry in " +
                                                         std::to_string(cfg_.retry_after.count()) + " s");
        }
        if (auto f = st_->fit(s, req); !f) {
            ++st_->rejected_memory;
            return f.error();
        }
        ++st_->in_flight;
        ++st_->admitted;
    }
    return Ticket(st_);
}

Result<void> AdmissionController::check_memory(const WorkRequest& req) {
    if (!cfg_.enabled)
        return {};
    const auto s = st_->sample();
    std::lock_guard lk(st_->m);
    st_->evaluate_locked(s);
    if (st_->level == PressureLevel::Critical) {
        ++st_->rejected_overloaded;
        return make_error(ErrorCode::Overloaded, "runtime is overloaded (free RAM below the hard floor of " +
                                                     mib(cfg_.hard_floor_bytes) + "): the " +
                                                     std::string(to_string(req.kind)) +
                                                     " was not started; retry in " +
                                                     std::to_string(cfg_.retry_after.count()) + " s");
    }
    if (auto f = st_->fit(s, req); !f) {
        ++st_->rejected_memory;
        return f.error();
    }
    return {};
}

PressureLevel AdmissionController::poll() {
    if (!cfg_.enabled)
        return PressureLevel::Normal;
    const auto s = st_->sample();
    bool do_shed = false;
    PressureLevel lvl;
    std::string reason;
    {
        std::lock_guard lk(st_->m);
        st_->evaluate_locked(s);
        lvl = st_->level;
        if (st_->shed_pending && lvl == PressureLevel::Critical) {
            st_->shed_pending = false;
            do_shed = true;
            reason = "watchdog: free RAM " +
                     (s.free_physical_bytes ? mib(*s.free_physical_bytes) : std::string("unknown")) +
                     " is below the hard floor of " + mib(cfg_.hard_floor_bytes);
            const auto frac = s.commit_fraction();
            if (frac && *frac > cfg_.hard_commit_fraction)
                reason = "watchdog: commit charge " + std::to_string(static_cast<int>(*frac * 100.0)) +
                         "% is above the hard limit of " +
                         std::to_string(static_cast<int>(cfg_.hard_commit_fraction * 100.0)) + "%";
        }
    }
    if (do_shed)
        (void)shed(reason);
    return lvl;
}

void AdmissionController::start_watchdog() {
    if (!cfg_.enabled || watchdog_.joinable())
        return;
    {
        std::lock_guard lk(st_->wd_m);
        st_->wd_stop = false;
    }
    {
        std::lock_guard lk(st_->m);
        st_->watchdog_running = true;
    }
    auto st = st_;
    const auto interval = cfg_.watchdog_interval;
    watchdog_ = std::thread([this, st, interval] {
        for (;;) {
            {
                std::unique_lock lk(st->wd_m);
                if (st->wd_cv.wait_for(lk, interval, [&] { return st->wd_stop; }))
                    return;
            }
            try {
                (void)poll();
            } catch (...) {
                // The watchdog must survive anything a probe or shed handler throws.
                (void)0;
            }
        }
    });
}

void AdmissionController::stop_watchdog() {
    if (!watchdog_.joinable())
        return;
    {
        std::lock_guard lk(st_->wd_m);
        st_->wd_stop = true;
    }
    st_->wd_cv.notify_all();
    watchdog_.join();
    std::lock_guard lk(st_->m);
    st_->watchdog_running = false;
}

void AdmissionController::set_shed_handler(ShedHandler handler) {
    std::lock_guard lk(st_->handler_m);
    st_->handler = std::move(handler);
}

void AdmissionController::clear_shed_handler() {
    std::lock_guard lk(st_->handler_m);  // waits for a handler that is currently running
    st_->handler = nullptr;
}

ShedReport AdmissionController::shed(const std::string& reason) {
    ShedReport rep;
    rep.reason = reason;
    {
        std::lock_guard hl(st_->handler_m);
        if (st_->handler) {
            try {
                rep = st_->handler(reason);
                rep.reason = reason;
            } catch (const std::exception& e) {
                rep.reason = reason + " (shed handler threw: " + e.what() + ")";
            }
        }
    }
    std::lock_guard lk(st_->m);
    ++st_->shed_count;
    st_->last_shed = to_json(rep);
    return rep;
}

PressureLevel AdmissionController::level() const {
    std::lock_guard lk(st_->m);
    return st_->level;
}

std::size_t AdmissionController::in_flight() const {
    std::lock_guard lk(st_->m);
    return st_->in_flight;
}

nlohmann::json AdmissionController::snapshot_json() {
    nlohmann::json j;
    j["enabled"] = cfg_.enabled;
    if (!cfg_.enabled)
        return j;
    const auto s = st_->sample();
    std::lock_guard lk(st_->m);
    st_->evaluate_locked(s);
    j["level"] = std::string(to_string(st_->level));
    j["sample"] = telemetry::to_json(s);
    j["in_flight"] = st_->in_flight;
    j["watchdog_running"] = st_->watchdog_running;
    j["counters"] = {{"admitted", st_->admitted},
                     {"rejected_out_of_memory", st_->rejected_memory},
                     {"rejected_overloaded", st_->rejected_overloaded},
                     {"shed_count", st_->shed_count}};
    j["limits"] = {{"min_free_ram_bytes", cfg_.min_free_ram_bytes},
                   {"hard_floor_bytes", cfg_.hard_floor_bytes},
                   {"hysteresis_bytes", cfg_.hysteresis_bytes},
                   {"max_commit_fraction", cfg_.max_commit_fraction},
                   {"hard_commit_fraction", cfg_.hard_commit_fraction},
                   {"max_in_flight", cfg_.max_in_flight},
                   {"retry_after_s", cfg_.retry_after.count()}};
    j["last_shed"] = st_->last_shed;
    return j;
}

std::uint64_t estimate_text_job_bytes(std::size_t prompt_bytes, std::uint32_t max_tokens) noexcept {
    const std::uint64_t per_prompt = checked_mul(prompt_bytes, 16).value_or(UINT64_MAX / 2);
    const std::uint64_t per_token = checked_mul(max_tokens, 8 * kKiB).value_or(UINT64_MAX / 2);
    return checked_add(checked_add(64 * kMiB, per_prompt).value_or(UINT64_MAX), per_token)
        .value_or(UINT64_MAX);
}

std::uint64_t estimate_vision_job_bytes(std::size_t encoded_image_bytes) noexcept {
    const std::uint64_t decoded =
        std::min<std::uint64_t>(checked_mul(encoded_image_bytes, 16).value_or(UINT64_MAX), 768 * kMiB);
    return 128 * kMiB + decoded;
}

std::uint64_t estimate_image_job_bytes(std::uint32_t width, std::uint32_t height,
                                       bool has_init_image) noexcept {
    const std::uint64_t px = static_cast<std::uint64_t>(width) * height;  // both <= 2^32: fits in 64 bits
    const std::uint64_t per_px = 6 * kKiB + (has_init_image ? 12 : 0);
    return checked_add(256 * kMiB, checked_mul(px, per_px).value_or(UINT64_MAX / 2)).value_or(UINT64_MAX);
}

}  // namespace hcs::scheduler
