#include "hcs/storage/stream.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "hcs/checked_math.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace hcs::storage {
namespace {

using Clock = std::chrono::steady_clock;

enum class SlotState { Free, Reading, Read, Decoding, Ready };

struct Slot {
    SlotState state = SlotState::Free;
    std::size_t chunk = 0;
    std::unique_ptr<std::uint8_t[]> stored;
    std::unique_ptr<std::uint8_t[]> raw;  // only allocated when some chunk in range is not Raw
    const std::uint8_t* data = nullptr;
    std::size_t data_size = 0;
};

class Pipeline {
public:
    Pipeline(const FileSource& src, std::span<const ChunkEntry> chunks, const StreamOptions& o, const ChunkSink& sink)
        : src_(src), chunks_(chunks), opt_(o), sink_(sink) {}
    ~Pipeline() { shutdown(); }

    Result<StreamStats> run();

private:
    void io_thread();
    void worker_thread();
    void fail(Error e);
    void wake_io();
    void shutdown();
    Result<void> decode_slot(Slot& s);

    const FileSource& src_;
    std::span<const ChunkEntry> chunks_;
    StreamOptions opt_;
    const ChunkSink& sink_;

    std::size_t begin_ = 0, end_ = 0, nslots_ = 0;
    std::vector<Slot> slots_;

    std::mutex m_;
    std::condition_variable cv_work_, cv_ready_;
    std::deque<std::size_t> workq_;
    bool stop_ = false;
    bool failed_ = false;
    Error err_;
    std::atomic<std::uint64_t> stored_read_{0};

    std::thread io_;
    std::vector<std::thread> workers_;
    bool shut_ = false;

#ifdef _WIN32
    std::unique_ptr<FileSource> async_file_;
    HANDLE iocp_ = nullptr;
    std::vector<OVERLAPPED> ovs_;
#endif
};

void Pipeline::wake_io() {
#ifdef _WIN32
    if (iocp_) PostQueuedCompletionStatus(iocp_, 0, 1, nullptr);
#else
    cv_work_.notify_all();
#endif
}

void Pipeline::fail(Error e) {
    {
        std::lock_guard lk(m_);
        if (!failed_) {
            failed_ = true;
            err_ = std::move(e);
        }
        stop_ = true;
    }
    cv_ready_.notify_all();
    cv_work_.notify_all();
    wake_io();
}

void Pipeline::shutdown() {
    if (shut_) return;
    shut_ = true;
    {
        std::lock_guard lk(m_);
        stop_ = true;
    }
    cv_ready_.notify_all();
    cv_work_.notify_all();
    wake_io();
    if (io_.joinable()) io_.join();
    for (auto& t : workers_)
        if (t.joinable()) t.join();
#ifdef _WIN32
    if (iocp_) {
        CloseHandle(iocp_);
        iocp_ = nullptr;
    }
#endif
}

Result<void> Pipeline::decode_slot(Slot& s) {
    const ChunkEntry& e = chunks_[s.chunk];
    const std::size_t comp = static_cast<std::size_t>(e.comp_size);
    const std::size_t raw = static_cast<std::size_t>(e.raw_size);
    if (e.codec == Codec::Raw) {
        // Decode in place: the stored bytes are the data. Only verify.
        if (opt_.verify_hash && sha256(std::span<const std::uint8_t>(s.stored.get(), raw)) != e.hash)
            return Error(ErrorCode::ModelError, "HCSMODEL: chunk " + std::to_string(s.chunk) + " checksum mismatch (corrupt or tampered data)");
        s.data = s.stored.get();
        s.data_size = raw;
        return {};
    }
    auto r = decode_chunk(e, std::span<const std::uint8_t>(s.stored.get(), comp), std::span<std::uint8_t>(s.raw.get(), raw),
                          opt_.verify_hash);
    if (!r) return Error(r.error().code, r.error().message + " (chunk " + std::to_string(s.chunk) + ")");
    s.data = s.raw.get();
    s.data_size = raw;
    return {};
}

void Pipeline::worker_thread() {
    for (;;) {
        std::size_t idx;
        {
            std::unique_lock lk(m_);
            cv_work_.wait(lk, [&] { return stop_ || !workq_.empty(); });
            if (stop_) return;
            idx = workq_.front();
            workq_.pop_front();
            slots_[idx].state = SlotState::Decoding;
        }
        Result<void> r = Error(ErrorCode::Internal, "unreachable");
        try {
            r = decode_slot(slots_[idx]);
        } catch (const std::exception& ex) {
            r = Error(ErrorCode::Internal, std::string("decoder exception: ") + ex.what());
        }
        if (!r) {
            fail(r.error());
            return;
        }
        {
            std::lock_guard lk(m_);
            slots_[idx].state = SlotState::Ready;
        }
        cv_ready_.notify_all();
    }
}

#ifdef _WIN32

void Pipeline::io_thread() {
    const HANDLE h = static_cast<HANDLE>(async_file_->native_handle());
    std::size_t next = begin_;
    std::size_t pending = 0;
    bool io_cancelled = false;
    OVERLAPPED_ENTRY ents[16];
    for (;;) {
        bool stopping;
        {
            std::lock_guard lk(m_);
            stopping = stop_;
        }
        while (!stopping && next < end_) {
            const std::size_t si = (next - begin_) % nslots_;
            Slot& s = slots_[si];
            {
                std::lock_guard lk(m_);
                if (s.state != SlotState::Free) break;
                s.state = SlotState::Reading;
                s.chunk = next;
            }
            const ChunkEntry& e = chunks_[next];
            OVERLAPPED& ov = ovs_[si];
            std::memset(&ov, 0, sizeof(ov));
            ov.Offset = static_cast<DWORD>(e.offset & 0xFFFFFFFFu);
            ov.OffsetHigh = static_cast<DWORD>(e.offset >> 32);
            if (!ReadFile(h, s.stored.get(), static_cast<DWORD>(e.comp_size), nullptr, &ov)) {
                const DWORD ec = GetLastError();
                if (ec != ERROR_IO_PENDING) {
                    fail(Error(ErrorCode::IoError, "overlapped read failed (Win32 error " + std::to_string(ec) + ")"));
                    break;
                }
            }
            ++pending;
            ++next;
            std::lock_guard lk(m_);
            stopping = stop_;
        }
        {
            std::lock_guard lk(m_);
            stopping = stop_;
        }
        if (stopping && !io_cancelled) {
            CancelIoEx(h, nullptr);
            io_cancelled = true;
        }
        if (pending == 0 && (stopping || next >= end_)) break;

        ULONG n = 0;
        if (!GetQueuedCompletionStatusEx(iocp_, ents, 16, &n, INFINITE, FALSE)) {
            fail(Error(ErrorCode::IoError, "completion port failure (Win32 error " + std::to_string(GetLastError()) + ")"));
            continue;
        }
        for (ULONG k = 0; k < n; ++k) {
            OVERLAPPED* pov = ents[k].lpOverlapped;
            if (!pov) continue;  // wake-up packet
            const std::size_t si = static_cast<std::size_t>(pov - ovs_.data());
            --pending;
            DWORD got = 0;
            const BOOL ok = GetOverlappedResult(h, pov, &got, FALSE);
            Slot& s = slots_[si];
            if (!ok) {
                const DWORD ec = GetLastError();
                if (ec != ERROR_OPERATION_ABORTED)
                    fail(Error(ErrorCode::IoError, "read failed (Win32 error " + std::to_string(ec) + "); device removed or file truncated?"));
                continue;
            }
            if (got != chunks_[s.chunk].comp_size) {
                fail(Error(ErrorCode::IoError, "short read for chunk " + std::to_string(s.chunk) + " (truncated file?)"));
                continue;
            }
            stored_read_ += got;
            {
                std::lock_guard lk(m_);
                if (stop_) continue;
                s.state = SlotState::Read;
                workq_.push_back(si);
            }
            cv_work_.notify_one();
        }
    }
}

#else  // portable fallback: blocking reads on the I/O thread

void Pipeline::io_thread() {
    for (std::size_t next = begin_; next < end_; ++next) {
        const std::size_t si = (next - begin_) % nslots_;
        Slot& s = slots_[si];
        {
            std::unique_lock lk(m_);
            cv_work_.wait(lk, [&] { return stop_ || s.state == SlotState::Free; });
            if (stop_) return;
            s.state = SlotState::Reading;
            s.chunk = next;
        }
        const ChunkEntry& e = chunks_[next];
        if (auto r = src_.read_at(e.offset, s.stored.get(), static_cast<std::size_t>(e.comp_size)); !r) {
            fail(r.error());
            return;
        }
        stored_read_ += e.comp_size;
        {
            std::lock_guard lk(m_);
            s.state = SlotState::Read;
            workq_.push_back(si);
        }
        cv_work_.notify_all();
    }
}

#endif

Result<StreamStats> Pipeline::run() {
    const auto t0 = Clock::now();
    if (opt_.buffers == 0 || opt_.buffers > 64) return Error(ErrorCode::InvalidArgument, "buffers must be in [1, 64]");
    begin_ = opt_.begin_chunk;
    end_ = std::min(opt_.end_chunk, chunks_.size());
    if (begin_ > end_) return Error(ErrorCode::InvalidArgument, "begin_chunk beyond end_chunk");
    StreamStats stats;
    if (begin_ == end_) return stats;

    std::uint64_t max_stored = 0, max_raw = 0;
    bool need_raw_buf = false;
    for (std::size_t i = begin_; i < end_; ++i) {
        const ChunkEntry& e = chunks_[i];
        if (e.raw_size == 0 || e.raw_size > kMaxChunkRawSize || e.comp_size == 0 || e.comp_size > kMaxChunkRawSize + (kMaxChunkRawSize >> 8) + 1024)
            return Error(ErrorCode::InvalidArgument, "chunk size outside the pipeline caps");
        if (!range_within(e.offset, e.comp_size, src_.size()))
            return Error(ErrorCode::IoError, "chunk " + std::to_string(i) + " lies outside the file (truncated)");
        max_stored = std::max(max_stored, e.comp_size);
        max_raw = std::max(max_raw, e.raw_size);
        if (e.codec != Codec::Raw) need_raw_buf = true;
    }
    nslots_ = std::min<std::size_t>(opt_.buffers, end_ - begin_);
    const std::uint64_t per_slot = max_stored + (need_raw_buf ? max_raw : 0);
    const auto total = checked_mul(per_slot, nslots_);
    if (!total) return Error(ErrorCode::OutOfMemory, "pipeline buffer size overflow");
    if (opt_.max_buffer_bytes && *total > opt_.max_buffer_bytes)
        return Error(ErrorCode::OutOfMemory, "pipeline buffers (" + std::to_string(*total) + " bytes) exceed the budget (" +
                                                 std::to_string(opt_.max_buffer_bytes) + " bytes); reduce buffers or chunk size");
    try {
        slots_.resize(nslots_);
        for (auto& s : slots_) {
            s.stored = std::make_unique_for_overwrite<std::uint8_t[]>(static_cast<std::size_t>(max_stored));
            if (need_raw_buf) s.raw = std::make_unique_for_overwrite<std::uint8_t[]>(static_cast<std::size_t>(max_raw));
        }
    } catch (const std::bad_alloc&) {
        return Error(ErrorCode::OutOfMemory, "cannot allocate streaming buffers");
    }
    stats.buffer_bytes_allocated = *total;

#ifdef _WIN32
    auto af = FileSource::open(src_.path(), opt_.sequential_hint);
    if (!af) return af.error();
    async_file_ = std::move(af).value();
    if (async_file_->size() != src_.size()) return Error(ErrorCode::IoError, "file changed while opening the streaming handle");
    iocp_ = CreateIoCompletionPort(static_cast<HANDLE>(async_file_->native_handle()), nullptr, 0, 1);
    if (!iocp_) return Error(ErrorCode::IoError, "CreateIoCompletionPort failed");
    ovs_.assign(nslots_, OVERLAPPED{});
#endif

    std::size_t nworkers = opt_.workers ? opt_.workers : std::min<std::size_t>(nslots_, 3);
    nworkers = std::clamp<std::size_t>(nworkers, 1, 16);
    try {
        for (std::size_t i = 0; i < nworkers; ++i) workers_.emplace_back([this] { worker_thread(); });
        io_ = std::thread([this] { io_thread(); });
    } catch (const std::system_error& ex) {
        shutdown();
        return Error(ErrorCode::Internal, std::string("cannot start pipeline threads: ") + ex.what());
    }

    double wait_s = 0;
    Result<void> outcome;
    for (std::size_t j = begin_; j < end_; ++j) {
        const std::size_t si = (j - begin_) % nslots_;
        Slot& s = slots_[si];
        {
            std::unique_lock lk(m_);
            const auto w0 = Clock::now();
            while (!(failed_ || (s.state == SlotState::Ready && s.chunk == j))) {
                if (opt_.cancel.cancelled()) {
                    lk.unlock();
                    fail(Error(ErrorCode::Cancelled, "streaming cancelled"));
                    lk.lock();
                    break;
                }
                cv_ready_.wait_for(lk, std::chrono::milliseconds(20));
            }
            wait_s += std::chrono::duration<double>(Clock::now() - w0).count();
            if (failed_) {
                outcome = err_;
                break;
            }
        }
        Result<void> r;
        try {
            r = sink_(j, chunks_[j], std::span<const std::uint8_t>(s.data, s.data_size));
        } catch (const std::exception& ex) {
            r = Error(ErrorCode::Internal, std::string("sink exception: ") + ex.what());
        }
        if (!r) {
            outcome = r.error();
            fail(r.error());
            break;
        }
        stats.chunks += 1;
        stats.raw_bytes += s.data_size;
        {
            std::lock_guard lk(m_);
            s.state = SlotState::Free;
            s.data = nullptr;
        }
        cv_work_.notify_all();
        wake_io();
        if (opt_.cancel.cancelled() && j + 1 < end_) {
            outcome = Error(ErrorCode::Cancelled, "streaming cancelled");
            fail(outcome.error());
            break;
        }
    }
    shutdown();  // joins all threads; outstanding overlapped reads are cancelled and drained first
    if (!outcome) return outcome.error();
    stats.stored_bytes_read = stored_read_.load();
    stats.wall_seconds = std::chrono::duration<double>(Clock::now() - t0).count();
    stats.consumer_wait_seconds = wait_s;
    return stats;
}

}  // namespace

Result<StreamStats> stream_chunks(const FileSource& file, std::span<const ChunkEntry> chunks, const StreamOptions& options,
                                  const ChunkSink& sink) {
    if (!sink) return Error(ErrorCode::InvalidArgument, "sink must not be empty");
    try {
        Pipeline p(file, chunks, options, sink);
        return p.run();
    } catch (const std::bad_alloc&) {
        return Error(ErrorCode::OutOfMemory, "out of memory in streaming pipeline");
    }
}

}  // namespace hcs::storage
