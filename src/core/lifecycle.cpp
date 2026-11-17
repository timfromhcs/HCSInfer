#include "hcs/lifecycle.hpp"

namespace hcs {

std::string_view to_string(ModelState s) noexcept {
    switch (s) {
        case ModelState::Unregistered:
            return "Unregistered";
        case ModelState::Discovered:
            return "Discovered";
        case ModelState::Validating:
            return "Validating";
        case ModelState::ReadyToLoad:
            return "ReadyToLoad";
        case ModelState::Loading:
            return "Loading";
        case ModelState::Loaded:
            return "Loaded";
        case ModelState::Running:
            return "Running";
        case ModelState::Unloading:
            return "Unloading";
        case ModelState::Unloaded:
            return "Unloaded";
        case ModelState::Failed:
            return "Failed";
    }
    return "Unknown";
}

std::string_view to_string(FailureKind k) noexcept {
    switch (k) {
        case FailureKind::None:
            return "none";
        case FailureKind::Recoverable:
            return "recoverable";
        case FailureKind::NonRecoverable:
            return "non_recoverable";
    }
    return "unknown";
}

bool is_legal_transition(ModelState from, ModelState to) noexcept {
    using S = ModelState;
    switch (from) {
        case S::Unregistered:
            return to == S::Discovered;
        case S::Discovered:
            return to == S::Validating;
        case S::Validating:
            return to == S::ReadyToLoad || to == S::Failed;
        case S::ReadyToLoad:
            return to == S::Loading || to == S::Validating;
        case S::Loading:
            return to == S::Loaded || to == S::Failed;
        case S::Loaded:
            return to == S::Running || to == S::Unloading || to == S::Failed;
        case S::Running:
            return to == S::Loaded || to == S::Unloading || to == S::Failed;
        case S::Unloading:
            return to == S::Unloaded || to == S::Failed;
        case S::Unloaded:
            return to == S::Loading || to == S::Validating;
        case S::Failed:
            return to == S::Validating || to == S::Unloading;  // kind-dependent, see below
    }
    return false;
}

ModelState ModelLifecycle::state() const {
    std::lock_guard lock(mutex_);
    return state_;
}
FailureKind ModelLifecycle::failure_kind() const {
    std::lock_guard lock(mutex_);
    return failure_kind_;
}
std::string ModelLifecycle::failure_reason() const {
    std::lock_guard lock(mutex_);
    return failure_reason_;
}
std::vector<ModelState> ModelLifecycle::history() const {
    std::lock_guard lock(mutex_);
    return history_;
}

Result<void> ModelLifecycle::transition_locked(ModelState to) {
    if (to == ModelState::Failed) {
        return make_error(ErrorCode::InvalidArgument, "use fail() to enter the Failed state");
    }
    if (!is_legal_transition(state_, to)) {
        return make_error(ErrorCode::InvalidState, "illegal model transition " +
                                                       std::string(to_string(state_)) + " -> " +
                                                       std::string(to_string(to)));
    }
    if (state_ == ModelState::Failed && failure_kind_ == FailureKind::NonRecoverable &&
        to != ModelState::Unloading) {
        return make_error(ErrorCode::InvalidState,
                          "non-recoverable failure: only Unloading is allowed (release resources)");
    }
    if (state_ == ModelState::Failed) {
        failure_kind_ = FailureKind::None;
        failure_reason_.clear();
    }
    state_ = to;
    history_.push_back(to);
    return {};
}

Result<void> ModelLifecycle::transition(ModelState to) {
    std::lock_guard lock(mutex_);
    return transition_locked(to);
}

Result<void> ModelLifecycle::transition_from(ModelState expected, ModelState to) {
    std::lock_guard lock(mutex_);
    if (state_ != expected) {
        return make_error(ErrorCode::InvalidState, "expected state " + std::string(to_string(expected)) +
                                                       " but model is " + std::string(to_string(state_)));
    }
    return transition_locked(to);
}

Result<void> ModelLifecycle::fail(FailureKind kind, std::string reason) {
    std::lock_guard lock(mutex_);
    if (kind == FailureKind::None) {
        return make_error(ErrorCode::InvalidArgument, "failure kind must be Recoverable or NonRecoverable");
    }
    if (!is_legal_transition(state_, ModelState::Failed)) {
        return make_error(ErrorCode::InvalidState,
                          "cannot fail from state " + std::string(to_string(state_)));
    }
    state_ = ModelState::Failed;
    failure_kind_ = kind;
    failure_reason_ = std::move(reason);
    history_.push_back(ModelState::Failed);
    return {};
}

}  // namespace hcs
