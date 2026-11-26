#pragma once
// Overload protection (plan F1): admission control, backpressure signals, kill switch and watchdog.
//
// The AdmissionController sits behind the control plane (hcs::api::ControlPlane); engines never see it.
//   * admit()          : every inference job (text / vision / image) and model-load job takes a Ticket first.
//                        It is refused with a structured error when the machine is under memory pressure,
//                        the predicted need would break the RAM floor / commit limit, or too many jobs are
//                        already in flight. The Ticket is RAII: dropping it frees the concurrency slot.
//   * check_memory()   : the memory-only check used inside a model load (the weights are known there).
//   * poll()/watchdog  : samples live memory; on entering the Critical level (free RAM below the hard floor)
//                        the registered shed handler (cancel every job, unload the model) runs once.
//   * shed()           : the manual kill switch (API route / MCP tool).
//
// Unknown measurements (nullopt) are never guessed: a missing figure disables only the rule that needs it
// and is reported as "unknown" in snapshot_json().
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <thread>

#include "hcs/result.hpp"
#include "hcs/telemetry/resource_probe.hpp"

namespace hcs::scheduler {

enum class WorkKind { ModelLoad, Text, Vision, Image, Benchmark };
std::string_view to_string(WorkKind k) noexcept;

enum class PressureLevel { Normal, Pressure, Critical };
std::string_view to_string(PressureLevel l) noexcept;

struct AdmissionConfig {
    bool enabled = true;
    // Soft floor: a request is refused when free physical RAM after its predicted need would drop below this.
    std::uint64_t min_free_ram_bytes = 2560ull << 20;  // 2.5 GiB
    // Soft limit on commit charge (used / limit), including the predicted need.
    double max_commit_fraction = 0.90;
    // Hard floor / limit: below / above these the controller is Critical and the watchdog sheds load.
    std::uint64_t hard_floor_bytes = 1024ull << 20;  // 1 GiB
    double hard_commit_fraction = 0.97;
    // Hysteresis: a level is left only after free RAM is this far above its threshold (and commit this far
    // below), so a reading hovering around a threshold does not flap between admit and refuse.
    std::uint64_t hysteresis_bytes = 512ull << 20;
    double hysteresis_commit_fraction = 0.02;
    // Jobs admitted and not yet finished (running + queued). The job queue has one worker, so the queue
    // length is max_in_flight - 1.
    std::size_t max_in_flight = 4;
    std::chrono::seconds retry_after{5};  // Retry-After hint for refused requests
    std::chrono::milliseconds watchdog_interval{1000};
    std::chrono::milliseconds shed_deadline{10000};  // bound for the kill switch
    bool check_gpu_heap = true;
};

// InvalidArgument with a precise message when the thresholds are inconsistent (e.g. hard floor above the soft
// floor, fractions outside (0,1]).
Result<void> validate(const AdmissionConfig& c);

struct WorkRequest {
    WorkKind kind = WorkKind::Text;
    std::string what;                       // model id / route name (for messages only)
    std::uint64_t predicted_bytes = 0;      // host RAM the work will additionally need
    std::uint64_t predicted_gpu_bytes = 0;  // GPU-resident part (checked against the heap headroom)
};

struct ShedReport {
    std::string reason;
    std::size_t jobs_cancelled = 0;
    bool model_unloaded = false;
    bool unload_pending = false;  // the unload did not finish inside the deadline and is still running
    bool jobs_idle = true;        // every cancelled job reached a terminal state inside the deadline
    std::int64_t duration_ms = 0;
};
nlohmann::json to_json(const ShedReport& r);
using ShedHandler = std::function<ShedReport(const std::string& reason)>;

namespace detail {
struct State;
}

// RAII concurrency slot. Movable, not copyable; typically held by the job closure in a shared_ptr.
class Ticket {
public:
    Ticket() = default;
    Ticket(Ticket&& o) noexcept : state_(std::move(o.state_)) {}
    Ticket& operator=(Ticket&& o) noexcept;
    Ticket(const Ticket&) = delete;
    Ticket& operator=(const Ticket&) = delete;
    ~Ticket() { release(); }
    explicit operator bool() const noexcept { return state_ != nullptr; }
    void release() noexcept;

private:
    friend class AdmissionController;
    explicit Ticket(std::shared_ptr<detail::State> s) : state_(std::move(s)) {}
    std::shared_ptr<detail::State> state_;
};

class AdmissionController {
public:
    AdmissionController(AdmissionConfig config, telemetry::ResourceProbe probe);
    ~AdmissionController();  // stops the watchdog
    AdmissionController(const AdmissionController&) = delete;
    AdmissionController& operator=(const AdmissionController&) = delete;

    // out_of_memory: the predicted need would break the RAM floor / commit limit / GPU headroom.
    // overloaded  : Critical or latched Pressure level, or max_in_flight reached.
    Result<Ticket> admit(const WorkRequest& req);
    // Memory-only check (no concurrency slot): used inside model loads. A request with predicted_bytes == 0
    // only fails in the Critical level.
    Result<void> check_memory(const WorkRequest& req);

    // One watchdog step: sample, update the level, run the shed handler when Critical was just entered.
    PressureLevel poll();
    void start_watchdog();  // idempotent; no-op when disabled
    void stop_watchdog();   // idempotent

    // Kill switch. Records the report in snapshot_json(); with no handler registered nothing is shed.
    ShedReport shed(const std::string& reason);
    // The handler is invoked outside the controller's locks. clear_shed_handler() waits for a running one.
    void set_shed_handler(ShedHandler handler);
    void clear_shed_handler();

    PressureLevel level() const;
    std::size_t in_flight() const;
    const AdmissionConfig& config() const noexcept { return cfg_; }
    int retry_after_seconds() const noexcept { return static_cast<int>(cfg_.retry_after.count()); }
    nlohmann::json snapshot_json();  // takes a fresh sample

private:
    AdmissionConfig cfg_;
    std::shared_ptr<detail::State> st_;
    std::thread watchdog_;
};

// Heuristic memory predictions for a job's working set (weights are accounted at model-load time).
// Deliberately upper-ish estimates, saturating, pure so they can be unit-tested.
std::uint64_t estimate_text_job_bytes(std::size_t prompt_bytes, std::uint32_t max_tokens) noexcept;
std::uint64_t estimate_vision_job_bytes(std::size_t encoded_image_bytes) noexcept;
std::uint64_t estimate_image_job_bytes(std::uint32_t width, std::uint32_t height,
                                       bool has_init_image) noexcept;

}  // namespace hcs::scheduler
