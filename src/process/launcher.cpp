#include "hcs/process/launcher.hpp"

#include <utility>

#include "hcs/process/child_process.hpp"

namespace hcs::process {

namespace {

class NativeChild final : public IChildProcess {
public:
    explicit NativeChild(ChildProcess child) : child_(std::move(child)) {}
    // ~ChildProcess kills the tree (kill-on-close), so a dropped child never outlives its owner.

    std::uint32_t pid() const override { return child_.pid(); }
    std::size_t read_stdout(char* buffer, std::size_t capacity) override {
        return child_.read_stdout(buffer, capacity);
    }
    std::size_t read_stderr(char* buffer, std::size_t capacity) override {
        return child_.read_stderr(buffer, capacity);
    }
    void terminate() override { (void)child_.terminate(); }
    std::optional<int> wait(std::uint32_t timeout_ms) override {
        auto waited = child_.wait_for(timeout_ms);
        if (!waited || waited.value().state != WaitState::Exited) {
            return std::nullopt;
        }
        return waited.value().exit_code;
    }

private:
    ChildProcess child_;
};

class NativeLauncher final : public IProcessLauncher {
public:
    Result<std::unique_ptr<IChildProcess>> launch(const LaunchSpec& spec) override {
        SpawnOptions options;
        options.cwd = spec.cwd;
        options.args = spec.args;
        options.env = spec.env;
        options.inherit_environment = false;
        options.stream_output = true;
        options.restrict_ui = true;
        options.max_active_processes = spec.active_process_limit == 0 ? 1 : spec.active_process_limit;
        options.max_job_memory_bytes = spec.memory_limit_bytes;
        auto child = ChildProcess::spawn(spec.exe, options);
        if (!child) {
            return child.error();
        }
        return std::unique_ptr<IChildProcess>(new NativeChild(std::move(child).value()));
    }
};

}  // namespace

std::unique_ptr<IProcessLauncher> make_native_launcher() {
    return std::make_unique<NativeLauncher>();
}

}  // namespace hcs::process
