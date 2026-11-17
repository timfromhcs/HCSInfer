#pragma once
// Model lifecycle state machine (AGENTS.md 10.3).
//
//  Unregistered -> Discovered -> Validating -> ReadyToLoad -> Loading -> Loaded
//  Loaded <-> Running ; Loaded|Running -> Unloading -> Unloaded -> Loading|Validating
//  Validating|Loading|Loaded|Running|Unloading -> Failed(recoverable|non-recoverable)
//  Failed(recoverable) -> Validating|Unloading ; Failed(non-recoverable) -> Unloading
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "hcs/result.hpp"

namespace hcs {

enum class ModelState {
    Unregistered,
    Discovered,
    Validating,
    ReadyToLoad,
    Loading,
    Loaded,
    Running,
    Unloading,
    Unloaded,
    Failed,
};

enum class FailureKind { None, Recoverable, NonRecoverable };

std::string_view to_string(ModelState s) noexcept;
std::string_view to_string(FailureKind k) noexcept;

// Static transition table (ignores failure kind; see ModelLifecycle for the Failed rules).
bool is_legal_transition(ModelState from, ModelState to) noexcept;

// Thread-safe: a single mutex serialises every mutation so exactly one owner wins a race.
class ModelLifecycle {
public:
    ModelState state() const;
    FailureKind failure_kind() const;
    std::string failure_reason() const;
    std::vector<ModelState> history() const;

    // Moves to `to`. Failed must be entered with fail(), never with transition().
    Result<void> transition(ModelState to);
    // Compare-and-transition: succeeds only if the current state is `expected`.
    Result<void> transition_from(ModelState expected, ModelState to);
    // Enters Failed (only legal from Validating/Loading/Loaded/Running/Unloading).
    Result<void> fail(FailureKind kind, std::string reason);

private:
    Result<void> transition_locked(ModelState to);

    mutable std::mutex mutex_;
    ModelState state_ = ModelState::Unregistered;
    FailureKind failure_kind_ = FailureKind::None;
    std::string failure_reason_;
    std::vector<ModelState> history_{ModelState::Unregistered};
};

}  // namespace hcs
