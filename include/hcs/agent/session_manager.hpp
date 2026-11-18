#pragma once
// HCS agent session manager (ADR-011): starts, supervises, streams and cancels prime-agent child processes.
//
// One session = one `prime-agent --mode json -p` child in a Windows Job Object, working directory confined to
// an approved root (PathGuard), environment rebuilt from scratch, every tool call audited (ToolAudit
// tripwire) and logged as metadata, output and event counts bounded, wall-clock timeout, cancellation kills
// the process tree. The child talks to the local HCS OpenAI-compatible endpoint with a bearer token passed
// ONLY through the child's environment (never argv, never a file, never a log). The command line and provider
// document come from hcs::agent::build_launch_plan / provider_config_json (agent_config.hpp); the single
// Job-Object launcher lives in hcs::process (launcher.hpp) and is injected so tests can substitute a scripted
// fake.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "hcs/agent/agent_config.hpp"
#include "hcs/agent/tool_audit.hpp"
#include "hcs/logger.hpp"
#include "hcs/path_guard.hpp"
#include "hcs/process/launcher.hpp"
#include "hcs/result.hpp"

namespace hcs::agent {

enum class SessionState { Starting, Running, Completed, Failed, Cancelled, TimedOut, PolicyKilled };
std::string_view to_string(SessionState s) noexcept;
constexpr bool is_terminal(SessionState s) noexcept {
    return s != SessionState::Starting && s != SessionState::Running;
}

struct SessionLimits {
    std::uint32_t max_concurrent = 1;    // the local model serves one request at a time
    std::size_t max_sessions_kept = 32;  // finished sessions kept in memory
    std::uint32_t default_timeout_ms = 15 * 60 * 1000;
    std::uint32_t max_timeout_ms = 2 * 60 * 60 * 1000;
    std::uint32_t kill_grace_ms = 3000;  // wait for exit after terminate
    std::size_t max_task_bytes = std::size_t{8} * 1024;
    std::size_t max_events = 4000;                             // per session (the final event is always kept)
    std::size_t max_event_text_bytes = std::size_t{8} * 1024;  // per tool payload / result text in an event
    std::size_t max_total_event_bytes = std::size_t{4} * 1024 * 1024;
    std::size_t max_line_bytes = std::size_t{1024} * 1024;  // longest accepted line of agent output
    std::uint32_t job_active_processes = 32;
    std::uint64_t job_memory_bytes = std::uint64_t{3} * 1024 * 1024 * 1024;
};

struct AgentSettings {
    std::filesystem::path agent_exe;      // prime-agent.exe (built from external/prime-agent)
    std::filesystem::path kernel_python;  // python.exe of the HCS kernel venv (runtime + default packages)
    std::filesystem::path state_dir;      // <hcs state>/agent: models.json, home, per-session logs
    std::string base_url;                 // e.g. http://127.0.0.1:8765/v1
    std::string token;                    // HCS bearer token; reaches the child only via its environment
    std::string model_id;                 // HCS model id, e.g. lfm3-omni-2b-q8_0
    std::string system_prompt;            // empty = default_agent_system_prompt()
    std::uint32_t context_window = 5120;  // advertised to prime-agent (must match the loaded HCS context)
    std::uint32_t max_tokens = 1024;
    bool persist_events = true;  // <state>/sessions/<id>/events.jsonl (content, capped, redacted)
    SessionLimits limits;
    AuditPolicy audit = default_audit_policy();
    bool enforce_audit = true;  // false = record violations but do not kill (tests / diagnostics only)
};

// Short prompt that replaces prime-agent's 42 KB default so a 2B model with a ~5K context can work
// (docs/agent.md).
std::string default_agent_system_prompt();

struct CreateRequest {
    std::filesystem::path root;  // workspace; must be inside an approved agent root
    std::string task;
    std::optional<std::uint32_t> timeout_ms;
};

struct AgentEvent {
    std::uint64_t seq = 0;   // starts at 1
    std::int64_t ts_ms = 0;  // unix ms
    std::string kind;
    nlohmann::json data;
};

struct SessionInfo {
    std::string id;
    SessionState state = SessionState::Starting;
    std::string root;
    std::string task_preview;  // first 120 bytes of the task (redacted)
    std::int64_t created_ms = 0;
    std::int64_t ended_ms = 0;
    std::optional<int> exit_code;
    std::uint64_t event_count = 0;
    std::uint32_t tool_calls = 0;
    std::uint32_t violations = 0;
    std::string end_reason;
};

struct EventPage {
    std::vector<AgentEvent> events;
    std::uint64_t next_seq = 1;  // pass as `after` to continue
    bool finished = false;       // session is terminal and every event has been delivered
    SessionState state = SessionState::Starting;
};

nlohmann::json to_json(const SessionInfo& s);
nlohmann::json to_json(const AgentEvent& e);

class SessionManager {
public:
    SessionManager(AgentSettings settings, PathGuard approved_roots,
                   std::unique_ptr<process::IProcessLauncher> launcher, Logger& logger);
    ~SessionManager();
    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    Result<std::string> create(const CreateRequest& request);
    std::vector<SessionInfo> list() const;
    Result<SessionInfo> get(const std::string& id) const;
    // Events with seq > after. Waits up to wait_ms when there are none and the session is still running.
    Result<EventPage> events(const std::string& id, std::uint64_t after, std::uint32_t wait_ms,
                             std::size_t max_count = 500) const;
    Result<void> cancel(const std::string& id);
    void shutdown();  // cancels and joins every session; idempotent

    const AgentSettings& settings() const noexcept { return settings_; }
    const PathGuard& approved_roots() const noexcept { return roots_; }

private:
    struct Session;
    void run_session(const std::shared_ptr<Session>& s, std::uint32_t timeout_ms);
    void watchdog();
    void push(Session& s, std::string kind, nlohmann::json data);
    void finish(Session& s, SessionState state, std::optional<int> exit_code, std::string reason);
    Result<std::map<std::string, std::string>> build_env(const std::filesystem::path& session_dir) const;
    ProviderConfig provider_config() const;
    Result<void> write_models_json() const;
    std::filesystem::path agent_dir() const { return settings_.state_dir / "agentdir"; }
    std::shared_ptr<Session> find(const std::string& id) const;
    std::string new_id();

    AgentSettings settings_;
    PathGuard roots_;
    std::unique_ptr<process::IProcessLauncher> launcher_;
    Logger& log_;

    mutable std::mutex mu_;
    std::map<std::string, std::shared_ptr<Session>> sessions_;
    std::vector<std::string> order_;  // creation order
    std::thread watchdog_;
    std::atomic<bool> stopping_{false};
    std::condition_variable wd_cv_;
    std::uint64_t rng_state_ = 0;
};

}  // namespace hcs::agent
