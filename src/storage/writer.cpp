#include "hcs/storage/writer.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <system_error>
#include <thread>

#include "hcs/checked_math.hpp"
#include "hcs/storage/file_source.hpp"

namespace hcs::storage {
namespace {

using Clock = std::chrono::steady_clock;

struct FileCloser {
    void operator()(std::FILE* f) const {
        if (f)
            std::fclose(f);
    }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

struct Job {
    std::vector<std::uint8_t> raw;
    std::vector<std::uint8_t> comp;  // compressed candidate
    bool use_comp = false;
    ChunkEntry entry;
    double cpu_seconds = 0;
    Result<void> status;
};

void process(Job& j, const WriteOptions& o) {
    const auto t0 = Clock::now();
    j.entry.raw_size = j.raw.size();
    j.entry.hash = sha256(j.raw);
    j.use_comp = false;
    if (o.codec != Codec::Raw) {
        j.status = compress(o.codec, o.level, j.raw, j.comp);
        if (j.status) {
            const double limit = static_cast<double>(j.raw.size()) * (1.0 - o.min_savings);
            if (static_cast<double>(j.comp.size()) <= limit)
                j.use_comp = true;
        }
    }
    j.entry.codec = j.use_comp ? o.codec : Codec::Raw;
    j.entry.comp_size = j.use_comp ? j.comp.size() : j.raw.size();
    j.cpu_seconds = std::chrono::duration<double>(Clock::now() - t0).count();
}

}  // namespace

Result<WriteStats> write_hcsmodel(const std::filesystem::path& src, const std::filesystem::path& dst,
                                  const WriteOptions& o) {
    auto in = FileSource::open(src, true);
    if (!in)
        return in.error();
    return write_hcsmodel(*in.value(), src.stem().string(), dst, o);
}

Result<WriteStats> write_hcsmodel(const ByteSource& in, const std::string& name_hint,
                                  const std::filesystem::path& dst, const WriteOptions& o) {
    const auto t_start = Clock::now();
    if (o.chunk_size < kMinWriterChunkSize || o.chunk_size > kMaxChunkRawSize)
        return Error(ErrorCode::InvalidArgument, "chunk size must be within [64 KiB, 256 MiB]");
    if (o.min_savings < 0.0 || o.min_savings >= 1.0)
        return Error(ErrorCode::InvalidArgument, "min_savings must be in [0, 1)");
    if (o.model_id.size() >= kIdFieldSize || o.architecture.size() >= kTextFieldSize ||
        o.quantization.size() >= kTextFieldSize)
        return Error(ErrorCode::InvalidArgument, "model_id / architecture / quantization text too long");

    const std::uint64_t total = in.size();
    if (total == 0)
        return Error(ErrorCode::InvalidArgument, "source file is empty");
    const std::uint64_t nchunks = (total + o.chunk_size - 1) / o.chunk_size;
    if (nchunks > kMaxChunkCount)
        return Error(ErrorCode::InvalidArgument, "too many chunks; use a larger chunk size");

    std::error_code ec;
    if (std::filesystem::exists(dst, ec))
        return Error(ErrorCode::AlreadyExists, "destination already exists; refusing to overwrite");
    if (!dst.has_parent_path() || !std::filesystem::is_directory(dst.parent_path(), ec))
        return Error(ErrorCode::NotFound, "destination directory does not exist");

    FilePtr out(
#ifdef _WIN32
        _wfopen(dst.c_str(), L"wbx")
#else
        std::fopen(dst.c_str(), "wbx")
#endif
    );
    if (!out)
        return Error(ErrorCode::IoError, "cannot create destination file");
    // From here on, any failure removes the file this call created (and only that file).
    bool success = false;
    struct Cleanup {
        FilePtr& f;
        const std::filesystem::path& p;
        bool& ok;
        ~Cleanup() {
            if (!ok) {
                f.reset();
                std::error_code e;
                std::filesystem::remove(p, e);
            }
        }
    } cleanup{out, dst, success};

    std::vector<std::uint8_t> placeholder(kHeaderSize, 0);
    if (std::fwrite(placeholder.data(), 1, placeholder.size(), out.get()) != placeholder.size())
        return Error(ErrorCode::IoError, "write failed");
    std::uint64_t file_pos = kHeaderSize;

    const std::size_t hw = std::max(1u, std::thread::hardware_concurrency());
    const std::size_t nthreads =
        std::clamp<std::size_t>(o.threads ? o.threads : std::min<std::size_t>(hw, 4), 1, 32);

    WriteStats st;
    st.raw_bytes = total;
    std::vector<ChunkEntry> entries;
    entries.reserve(static_cast<std::size_t>(nchunks));
    Sha256 manifest;
    std::uint64_t done = 0;

    while (done < total) {
        if (o.cancel.cancelled())
            return Error(ErrorCode::Cancelled, "write cancelled");
        std::vector<Job> batch;
        const std::uint64_t remaining_chunks = (total - done + o.chunk_size - 1) / o.chunk_size;
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(nthreads, remaining_chunks));
        batch.resize(n);
        const auto r0 = Clock::now();
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint64_t len = std::min<std::uint64_t>(o.chunk_size, total - done);
            batch[i].raw.resize(static_cast<std::size_t>(len));
            if (auto r = in.read_at(done, batch[i].raw.data(), batch[i].raw.size()); !r)
                return r.error();
            manifest.update(batch[i].raw);
            batch[i].entry.raw_offset = done;
            done += len;
        }
        st.source_read_seconds += std::chrono::duration<double>(Clock::now() - r0).count();

        if (n == 1) {
            process(batch[0], o);
        } else {
            std::vector<std::thread> ts;
            ts.reserve(n);
            for (std::size_t i = 0; i < n; ++i) ts.emplace_back([&, i] { process(batch[i], o); });
            for (auto& t : ts) t.join();
        }
        for (auto& j : batch) {
            if (!j.status)
                return j.status.error();
            st.compress_cpu_seconds += j.cpu_seconds;
            j.entry.region_id = entries.size();
            j.entry.offset = file_pos;
            const auto& bytes = j.use_comp ? j.comp : j.raw;
            if (std::fwrite(bytes.data(), 1, bytes.size(), out.get()) != bytes.size())
                return Error(ErrorCode::IoError, "write failed (disk full?)");
            file_pos += bytes.size();
            st.payload_bytes += bytes.size();
            switch (j.entry.codec) {
                case Codec::Raw:
                    ++st.chunks_raw;
                    break;
                case Codec::Lz4:
                    ++st.chunks_lz4;
                    break;
                case Codec::Zstd:
                    ++st.chunks_zstd;
                    break;
            }
            entries.push_back(j.entry);
        }
    }

    HcsHeader h;
    h.nominal_chunk_size = o.chunk_size;
    h.chunk_table_offset = file_pos;
    h.chunk_count = entries.size();
    h.total_raw_size = total;
    h.model_id = o.model_id.empty() ? name_hint.substr(0, kIdFieldSize - 1) : o.model_id;
    h.architecture = o.architecture.empty() ? "unknown" : o.architecture;
    h.quantization = o.quantization.empty() ? "unknown" : o.quantization;
    h.source_manifest_hash = manifest.finalize();
    const auto table = serialize_chunk_table(entries);
    h.chunk_table_hash = sha256(table);
    if (std::fwrite(table.data(), 1, table.size(), out.get()) != table.size())
        return Error(ErrorCode::IoError, "write failed");
    file_pos += table.size();
    const auto hdr = serialize_header(h);
    if (
#ifdef _WIN32
        _fseeki64(out.get(), 0, SEEK_SET) != 0
#else
        std::fseek(out.get(), 0, SEEK_SET) != 0
#endif
        || std::fwrite(hdr.data(), 1, hdr.size(), out.get()) != hdr.size() || std::fflush(out.get()) != 0)
        return Error(ErrorCode::IoError, "write failed");
    if (std::fclose(out.release()) != 0)
        return Error(ErrorCode::IoError, "close failed");

    st.chunks = entries.size();
    st.container_bytes = file_pos;
    st.source_sha256_hex = to_hex(h.source_manifest_hash);
    st.seconds = std::chrono::duration<double>(Clock::now() - t_start).count();
    // NOLINTNEXTLINE(clang-analyzer-deadcode.DeadStores) - read by the Cleanup guard's destructor above
    success = true;
    return st;
}

}  // namespace hcs::storage
