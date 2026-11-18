#include "hcs/process/child_process.hpp"

#include <utility>

#ifdef _WIN32
#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <mutex>
#include <thread>
#endif

namespace hcs::process {

#ifdef _WIN32

namespace {

Error win_error(const std::string& what) {
    const DWORD code = GetLastError();
    return make_error(ErrorCode::IoError, what + " failed (Win32 error " + std::to_string(code) + ")");
}

Result<std::wstring> to_wide(const std::string& utf8) {
    if (utf8.empty()) {
        return std::wstring{};
    }
    const int size = static_cast<int>(utf8.size());
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), size, nullptr, 0);
    if (needed <= 0) {
        return win_error("MultiByteToWideChar");
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, utf8.data(), size, out.data(), needed) != needed) {
        return win_error("MultiByteToWideChar");
    }
    return out;
}

// The MSVCRT argv quoting rules ("Everyone quotes command line arguments the wrong way").
std::wstring quote_argument(const std::wstring& arg) {
    if (arg.empty()) {
        return L"\"\"";
    }
    if (arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out;
    out.push_back(L'"');
    for (std::size_t i = 0; i < arg.size(); ++i) {
        std::size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++backslashes;
            ++i;
        }
        if (i == arg.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(arg[i]);
        }
    }
    out.push_back(L'"');
    return out;
}

// Builds a full UTF-16 environment block from the parent environment plus `overrides`. Windows environment
// variable names are case-insensitive, so variables are keyed by upper-case name.
Result<std::wstring> build_environment_block(const std::map<std::string, std::string>& overrides,
                                             bool inherit) {
    std::map<std::wstring, std::wstring> variables;
    if (LPWCH block = inherit ? GetEnvironmentStringsW() : nullptr) {
        for (const wchar_t* cursor = block; *cursor != L'\0';) {
            const std::wstring entry(cursor);
            cursor += entry.size() + 1;
            if (entry.empty() || entry[0] == L'=') {
                continue;  // hidden per-drive variables ("=C:")
            }
            const std::size_t equals = entry.find(L'=');
            if (equals == std::wstring::npos) {
                continue;
            }
            std::wstring name = entry.substr(0, equals);
            std::transform(name.begin(), name.end(), name.begin(),
                           [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
            variables[name] = entry.substr(equals + 1);
        }
        FreeEnvironmentStringsW(block);
    }
    for (const auto& [name, value] : overrides) {
        auto name_w = to_wide(name);
        if (!name_w) {
            return name_w.error();
        }
        auto value_w = to_wide(value);
        if (!value_w) {
            return value_w.error();
        }
        std::wstring key = name_w.value();
        std::transform(key.begin(), key.end(), key.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
        variables[key] = value_w.value();
    }
    std::wstring out;
    for (const auto& [name, value] : variables) {
        out += name;
        out.push_back(L'=');
        out += value;
        out.push_back(L'\0');
    }
    out.push_back(L'\0');
    return out;
}

}  // namespace

struct ChildProcess::Impl {
    HANDLE job = nullptr;        // kill-on-close job object owning the tree
    HANDLE process = nullptr;    // process handle
    HANDLE pipe_read = nullptr;  // our end of the stdout (or merged-output) pipe (nullptr when not capturing)
    HANDLE err_read = nullptr;   // streaming mode: our end of the stderr pipe
    bool streaming = false;
    std::thread reader;
    mutable std::mutex mtx;
    std::mutex reader_mtx;
    mutable std::mutex state_mtx;  // guards exited / exit_code (terminate() may race with wait_for())
    std::string buffer;
    bool truncated = false;
    std::uint64_t max_output = 0;
    std::uint32_t pid = 0;
    bool exited = false;
    int exit_code = -1;

    bool has_exited() const {
        std::lock_guard<std::mutex> lock(state_mtx);
        return exited;
    }

    // Records the exit state once the process object is signalled (or the wait gave up).
    void record_exit() {
        DWORD code = 0;
        const bool got =
            process != nullptr && GetExitCodeProcess(process, &code) != 0 && code != STILL_ACTIVE;
        std::lock_guard<std::mutex> lock(state_mtx);
        if (!exited) {
            exited = true;
            exit_code = got ? static_cast<int>(code) : -1;
        }
    }

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl() { close_all(); }

    void drain() {
        char chunk[4096];
        for (;;) {
            DWORD read = 0;
            if (ReadFile(pipe_read, chunk, static_cast<DWORD>(sizeof(chunk)), &read, nullptr) == 0 ||
                read == 0) {
                break;
            }
            std::lock_guard<std::mutex> lock(mtx);
            const std::uint64_t room = max_output > buffer.size() ? max_output - buffer.size() : 0;
            const std::size_t keep =
                static_cast<std::size_t>((std::min)(room, static_cast<std::uint64_t>(read)));
            buffer.append(chunk, keep);
            if (keep < read) {
                truncated = true;
            }
        }
    }

    void kill_and_wait() {
        if (process != nullptr && !has_exited()) {
            if (job != nullptr) {
                TerminateJobObject(job, 1);
            }
            WaitForSingleObject(process, 10000);
            record_exit();
        }
    }

    void finish_reader() {
        std::lock_guard<std::mutex> lock(reader_mtx);  // terminate() and wait_for() may both get here
        if (reader.joinable()) {
            reader.join();
        }
    }

    void close_all() {
        kill_and_wait();
        finish_reader();
        if (pipe_read != nullptr) {
            CloseHandle(pipe_read);
            pipe_read = nullptr;
        }
        if (err_read != nullptr) {
            CloseHandle(err_read);
            err_read = nullptr;
        }
        if (process != nullptr) {
            CloseHandle(process);
            process = nullptr;
        }
        if (job != nullptr) {
            CloseHandle(job);  // kill-on-close: any survivor dies here
            job = nullptr;
        }
    }
};

#endif  // _WIN32

ChildProcess::ChildProcess() noexcept = default;
ChildProcess::ChildProcess(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
ChildProcess::~ChildProcess() = default;
ChildProcess::ChildProcess(ChildProcess&&) noexcept = default;
ChildProcess& ChildProcess::operator=(ChildProcess&&) noexcept = default;

Result<ChildProcess> ChildProcess::spawn(const std::filesystem::path& program, const SpawnOptions& options) {
#ifdef _WIN32
    if (!program.is_absolute() || program.filename().empty()) {
        return make_error(ErrorCode::InvalidArgument, "program must be an absolute path to a file");
    }
    if (!options.cwd.is_absolute()) {
        return make_error(ErrorCode::InvalidArgument, "cwd must be an absolute path");
    }
    auto program_w = to_wide(program.string());
    if (!program_w) {
        return program_w.error();
    }
    auto cwd_w = to_wide(options.cwd.string());
    if (!cwd_w) {
        return cwd_w.error();
    }

    std::wstring command_line = quote_argument(program_w.value());
    for (const auto& arg : options.args) {
        auto arg_w = to_wide(arg);
        if (!arg_w) {
            return arg_w.error();
        }
        command_line.push_back(L' ');
        command_line += quote_argument(arg_w.value());
    }
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    std::wstring environment_block;
    DWORD extra_flags = 0;
    if (!options.env.empty() || !options.inherit_environment) {
        auto env_block = build_environment_block(options.env, options.inherit_environment);
        if (!env_block) {
            return env_block.error();
        }
        environment_block = std::move(env_block).value();
        extra_flags = CREATE_UNICODE_ENVIRONMENT;
    }

    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    HANDLE err_read_end = nullptr;
    HANDLE err_write_end = nullptr;
    HANDLE nul_in = nullptr;
    const bool streaming = options.stream_output;
    // Releases every handle opened so far; used on each early-return path below.
    const auto close_pipes = [&] {
        for (HANDLE* h : {&read_end, &write_end, &err_read_end, &err_write_end, &nul_in}) {
            if (*h != nullptr) {
                CloseHandle(*h);
                *h = nullptr;
            }
        }
    };
    if (options.capture_output || streaming) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        if (!CreatePipe(&read_end, &write_end, &sa, 0)) {
            return win_error("CreatePipe");
        }
        if (!SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0)) {
            const Error failure = win_error("SetHandleInformation");
            close_pipes();
            return failure;
        }
        if (streaming) {
            if (!CreatePipe(&err_read_end, &err_write_end, &sa, 0)) {
                const Error failure = win_error("CreatePipe");
                close_pipes();
                return failure;
            }
            if (!SetHandleInformation(err_read_end, HANDLE_FLAG_INHERIT, 0)) {
                const Error failure = win_error("SetHandleInformation");
                close_pipes();
                return failure;
            }
            nul_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                                 0, nullptr);
            if (nul_in == INVALID_HANDLE_VALUE) {
                nul_in = nullptr;
                const Error failure = win_error("CreateFile(NUL)");
                close_pipes();
                return failure;
            }
        }
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) {
        const Error failure = win_error("CreateJobObject");
        close_pipes();
        return failure;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info{};
    job_info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (options.max_active_processes > 0) {
        job_info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        job_info.BasicLimitInformation.ActiveProcessLimit = options.max_active_processes;
    }
    if (options.max_job_memory_bytes > 0) {
        job_info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
        job_info.JobMemoryLimit = static_cast<SIZE_T>(options.max_job_memory_bytes);
    }
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &job_info, sizeof(job_info))) {
        const Error failure = win_error("SetInformationJobObject");
        CloseHandle(job);
        close_pipes();
        return failure;
    }
    if (options.restrict_ui) {
        JOBOBJECT_BASIC_UI_RESTRICTIONS ui{};
        ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_EXITWINDOWS |
                                 JOB_OBJECT_UILIMIT_GLOBALATOMS | JOB_OBJECT_UILIMIT_HANDLES |
                                 JOB_OBJECT_UILIMIT_READCLIPBOARD | JOB_OBJECT_UILIMIT_WRITECLIPBOARD |
                                 JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS;
        if (!SetInformationJobObject(job, JobObjectBasicUIRestrictions, &ui, sizeof(ui))) {
            const Error failure = win_error("SetInformationJobObject(UI)");
            CloseHandle(job);
            close_pipes();
            return failure;
        }
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul_in;
    si.StartupInfo.hStdOutput = write_end;
    si.StartupInfo.hStdError = streaming ? err_write_end : write_end;

    // Only the pipe handles are inherited: the child must not see HCSInfer's sockets or file handles.
    std::vector<HANDLE> inherited;
    if (write_end != nullptr) {
        inherited.push_back(write_end);
    }
    if (err_write_end != nullptr) {
        inherited.push_back(err_write_end);
    }
    if (nul_in != nullptr) {
        inherited.push_back(nul_in);
    }
    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    std::vector<char> attribute_buffer(attribute_size);
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_buffer.data());
    if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attribute_size)) {
        const Error failure = win_error("InitializeProcThreadAttributeList");
        CloseHandle(job);
        close_pipes();
        return failure;
    }
    if (!inherited.empty() &&
        !UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   static_cast<void*>(inherited.data()), inherited.size() * sizeof(HANDLE),
                                   nullptr, nullptr)) {
        const Error failure = win_error("UpdateProcThreadAttribute");
        DeleteProcThreadAttributeList(si.lpAttributeList);
        CloseHandle(job);
        close_pipes();
        return failure;
    }

    PROCESS_INFORMATION pi{};
    const DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW | extra_flags;
    const BOOL created = CreateProcessW(program_w.value().c_str(), mutable_command.data(), nullptr, nullptr,
                                        inherited.empty() ? FALSE : TRUE, flags,
                                        environment_block.empty() ? nullptr : environment_block.data(),
                                        cwd_w.value().c_str(), &si.StartupInfo, &pi);
    const Error create_failure = created ? Error{} : win_error("CreateProcessW");
    DeleteProcThreadAttributeList(si.lpAttributeList);
    // The child holds its own copies of these; closing ours is what lets the pipes reach EOF.
    for (HANDLE* h : {&write_end, &err_write_end, &nul_in}) {
        if (*h != nullptr) {
            CloseHandle(*h);
            *h = nullptr;
        }
    }
    if (!created) {
        CloseHandle(job);
        close_pipes();
        return create_failure;
    }

    if (!AssignProcessToJobObject(job, pi.hProcess)) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
        const Error failure = win_error("AssignProcessToJobObject");
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(job);
        close_pipes();
        return failure;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    auto impl = std::make_unique<Impl>();
    impl->job = job;
    impl->process = pi.hProcess;
    impl->pipe_read = read_end;
    impl->err_read = err_read_end;
    impl->streaming = streaming;
    impl->max_output = options.max_output_bytes;
    impl->pid = pi.dwProcessId;
    if (read_end != nullptr && !streaming) {
        Impl* raw = impl.get();
        impl->reader = std::thread([raw] { raw->drain(); });
    }
    return ChildProcess(std::move(impl));
#else
    (void)program;
    (void)options;
    return make_error(ErrorCode::Unsupported, "child processes are implemented on Windows only");
#endif
}

bool ChildProcess::running() const {
#ifdef _WIN32
    if (!impl_ || impl_->has_exited()) {
        return false;
    }
    return WaitForSingleObject(impl_->process, 0) == WAIT_TIMEOUT;
#else
    return false;
#endif
}

std::uint32_t ChildProcess::pid() const noexcept {
#ifdef _WIN32
    return impl_ ? impl_->pid : 0;
#else
    return 0;
#endif
}

Result<WaitResult> ChildProcess::wait_for(std::uint64_t timeout_ms) {
#ifdef _WIN32
    if (!impl_) {
        return make_error(ErrorCode::InvalidState, "no process");
    }
    if (!impl_->has_exited()) {
        const std::uint64_t capped = (std::min)(timeout_ms, static_cast<std::uint64_t>(0xFFFFFFFEu));
        const DWORD result = WaitForSingleObject(impl_->process, static_cast<DWORD>(capped));
        if (result == WAIT_TIMEOUT) {
            return WaitResult{WaitState::TimedOut, -1, impl_->pid};
        }
        if (result != WAIT_OBJECT_0) {
            return win_error("WaitForSingleObject");
        }
        impl_->record_exit();
    }
    impl_->finish_reader();  // the tree has exited, so the pipe is at EOF
    std::lock_guard<std::mutex> lock(impl_->state_mtx);
    return WaitResult{WaitState::Exited, impl_->exit_code, impl_->pid};
#else
    (void)timeout_ms;
    return make_error(ErrorCode::Unsupported, "child processes are implemented on Windows only");
#endif
}

Result<void> ChildProcess::terminate() {
#ifdef _WIN32
    if (!impl_) {
        return {};
    }
    impl_->kill_and_wait();
    impl_->finish_reader();
    return {};
#else
    return make_error(ErrorCode::Unsupported, "child processes are implemented on Windows only");
#endif
}

std::size_t ChildProcess::read_stdout(char* buffer, std::size_t capacity) {
#ifdef _WIN32
    if (!impl_ || !impl_->streaming || impl_->pipe_read == nullptr || capacity == 0) {
        return 0;
    }
    DWORD got = 0;
    if (ReadFile(impl_->pipe_read, buffer, static_cast<DWORD>((std::min)(capacity, std::size_t{1} << 20)),
                 &got, nullptr) == 0) {
        return 0;
    }
    return got;
#else
    (void)buffer;
    (void)capacity;
    return 0;
#endif
}

std::size_t ChildProcess::read_stderr(char* buffer, std::size_t capacity) {
#ifdef _WIN32
    if (!impl_ || !impl_->streaming || impl_->err_read == nullptr || capacity == 0) {
        return 0;
    }
    DWORD got = 0;
    if (ReadFile(impl_->err_read, buffer, static_cast<DWORD>((std::min)(capacity, std::size_t{1} << 20)),
                 &got, nullptr) == 0) {
        return 0;
    }
    return got;
#else
    (void)buffer;
    (void)capacity;
    return 0;
#endif
}

std::string ChildProcess::output() const {
#ifdef _WIN32
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(impl_->mtx);
    return impl_->buffer;
#else
    return {};
#endif
}

bool ChildProcess::output_truncated() const {
#ifdef _WIN32
    if (!impl_) {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mtx);
    return impl_->truncated;
#else
    return false;
#endif
}

}  // namespace hcs::process
