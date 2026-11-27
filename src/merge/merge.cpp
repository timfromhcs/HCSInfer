// File-level merge engine: validation, LoRA pair discovery, streaming writer with SHA-256, atomic finish.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>

#include "hcs/fs_util.hpp"
#include "hcs/merge.hpp"
#include "hcs/storage/sha256.hpp"
#include "tensor_file.hpp"

namespace hcs::merge {

using detail::DType;
using detail::Tensor;
using detail::TensorFile;
namespace fs = std::filesystem;

namespace {

Error err(ErrorCode c, std::string m) {
    return Error(c, std::move(m));
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool ends_with(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

// ---- output sink: file + running SHA-256
// ----------------------------------------------------------------------
class Sink {
public:
    Result<void> open(const fs::path& p) {
        out_.open(p, std::ios::binary | std::ios::trunc);
        if (!out_)
            return err(ErrorCode::IoError, "cannot create '" + p.string() + "'");
        return {};
    }
    Result<void> write(const void* data, std::size_t n) {
        if (n == 0)
            return {};
        out_.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
        if (!out_)
            return err(ErrorCode::IoError, "write failed (disk full or the drive was removed?)");
        hash_.update(data, n);
        written_ += n;
        return {};
    }
    Result<void> close() {
        out_.flush();
        const bool ok = static_cast<bool>(out_);
        out_.close();
        if (!ok)
            return err(ErrorCode::IoError, "flushing the output failed");
        return {};
    }
    std::uint64_t written() const { return written_; }
    std::string hex() { return storage::to_hex(hash_.finalize()); }

private:
    std::ofstream out_;
    storage::Sha256 hash_;
    std::uint64_t written_ = 0;
};

void encode(const float* in, std::size_t n, DType d, std::vector<std::uint8_t>& buf) {
    const std::size_t es = static_cast<std::size_t>(detail::elem_bytes(d));
    buf.resize(n * es);
    if (d == DType::F32) {
        std::memcpy(buf.data(), in, n * 4);
    } else if (d == DType::F16) {
        auto* p = reinterpret_cast<std::uint16_t*>(buf.data());
        for (std::size_t i = 0; i < n; ++i) p[i] = float_to_half(in[i]);
    } else {
        auto* p = reinterpret_cast<std::uint16_t*>(buf.data());
        for (std::size_t i = 0; i < n; ++i) p[i] = float_to_bf16(in[i]);
    }
}

bool same_shape(const Tensor& a, const Tensor& b) {
    return a.shape == b.shape;
}

std::string shape_str(const std::vector<std::uint64_t>& s) {
    std::string o = "[";
    for (std::size_t i = 0; i < s.size(); ++i) o += (i ? "," : "") + std::to_string(s[i]);
    return o + "]";
}

// ---- LoRA pair discovery
// --------------------------------------------------------------------------------------
struct LoraPair {
    std::string target;         // base tensor name
    const Tensor* a = nullptr;  // [rank, in]
    const Tensor* b = nullptr;  // [out, rank]
    std::optional<double> alpha;
};

Result<std::vector<LoraPair>> discover_lora_pairs(const TensorFile& lora, const TensorFile& base) {
    struct Slot {
        const Tensor *a = nullptr, *b = nullptr;
        std::string alpha_name;
    };
    std::map<std::string, Slot> slots;
    auto strip_peft = [](std::string s) {
        const std::string pre = "base_model.model.";
        if (s.rfind(pre, 0) == 0)
            s = s.substr(pre.size());
        return s;
    };
    for (const auto& t : lora.tensors()) {
        const std::string& n = t.name;
        if (n.rfind("lora_unet_", 0) == 0 || n.rfind("lora_te", 0) == 0)
            return err(
                ErrorCode::Unsupported,
                "LoRA tensor '" + n +
                    "' uses kohya underscore naming that cannot be mapped to base tensor "
                    "names; convert it to PEFT (…lora_A/lora_B.weight) or llama.cpp (…lora_a/lora_b) naming");
        auto set = [&](const std::string& suffix, bool is_a, const std::string& target_suffix,
                       bool alpha_by_prefix) {
            if (!ends_with(n, suffix))
                return false;
            std::string prefix = strip_peft(n.substr(0, n.size() - suffix.size()));
            const std::string target = prefix + target_suffix;
            Slot& s = slots[target];
            (is_a ? s.a : s.b) = &t;
            if (alpha_by_prefix)
                s.alpha_name = n.substr(0, n.size() - suffix.size()) + ".alpha";
            return true;
        };
        if (set(".lora_A.default.weight", true, ".weight", false) ||
            set(".lora_B.default.weight", false, ".weight", false) ||
            set(".lora_A.weight", true, ".weight", false) || set(".lora_B.weight", false, ".weight", false) ||
            set(".lora_down.weight", true, ".weight", true) ||
            set(".lora_up.weight", false, ".weight", true) || set(".lora_a", true, "", false) ||
            set(".lora_b", false, "", false))
            continue;
        if (ends_with(n, ".alpha"))
            continue;  // `<module>.alpha` scalars are read through the pair they belong to
        if (n == "__metadata__")
            continue;
        return err(ErrorCode::ModelError,
                   "LoRA file has a tensor this tool does not understand: '" + n + "'");
    }
    if (slots.empty())
        return err(ErrorCode::ModelError,
                   "the LoRA file contains no lora_A/lora_B (or lora_down/lora_up, lora_a/lora_b) pairs");
    std::vector<LoraPair> out;
    for (auto& [target, s] : slots) {
        if (!s.a || !s.b)
            return err(ErrorCode::ModelError,
                       "LoRA target '" + target + "' has only one half of its A/B pair");
        const Tensor* bt = base.find(target);
        if (!bt)
            return err(ErrorCode::ModelError,
                       "LoRA targets '" + target + "', which does not exist in the base model");
        if (!detail::is_float(bt->dtype))
            return err(ErrorCode::Unsupported, "LoRA target '" + target + "' is stored as " + bt->type_name +
                                                   " in the base: quantized targets are not merged in V1.1");
        if (bt->shape.size() != 2 || s.a->shape.size() != 2 || s.b->shape.size() != 2)
            return err(ErrorCode::Unsupported,
                       "LoRA target '" + target + "' is not a 2-D weight (conv LoRA is not supported)");
        if (!detail::is_float(s.a->dtype) || !detail::is_float(s.b->dtype))
            return err(ErrorCode::ModelError, "LoRA tensors for '" + target + "' are not float tensors");
        const std::uint64_t rank = s.a->shape[0];
        if (rank == 0 || s.b->shape[1] != rank || s.a->shape[1] != bt->shape[1] ||
            s.b->shape[0] != bt->shape[0])
            return err(ErrorCode::ModelError, "LoRA shapes for '" + target + "' (A " + shape_str(s.a->shape) +
                                                  ", B " + shape_str(s.b->shape) +
                                                  ") do not fit the base tensor " + shape_str(bt->shape));
        LoraPair p;
        p.target = target;
        p.a = s.a;
        p.b = s.b;
        if (!s.alpha_name.empty())
            if (const Tensor* at = lora.find(s.alpha_name);
                at && detail::is_float(at->dtype) && at->elements == 1) {
                float v = 0;
                if (auto r = lora.read_floats(*at, 0, 1, &v); r)
                    p.alpha = v;
            }
        out.push_back(p);
    }
    return out;
}

}  // namespace

Result<fs::path> validate_merge_output(const fs::path& out, const PathGuard& guard) {
    if (out.empty())
        return err(ErrorCode::InvalidArgument, "an output path is required");
    auto r = guard.resolve(out);
    if (!r)
        return r.error();
#ifdef _WIN32
    const std::string root = r.value().root_name().string();
    if (!root.empty() && (root[0] == 'C' || root[0] == 'c'))
        return err(ErrorCode::SecurityViolation, "refusing to write a merged model to drive C:");
#endif
    return r.value();
}

Result<Report> run_merge(const Request& req, const PathGuard& guard) {
    const auto t0 = std::chrono::steady_clock::now();
    const Params& P = req.params;
    const bool is_lora = P.method == Method::Lora;
    const bool is_task = P.method == Method::Ties || P.method == Method::Dare;

    // ---- parameter validation (no I/O yet) ----
    if (req.models.empty())
        return err(ErrorCode::InvalidArgument, "at least one --model is required");
    if (P.method == Method::Slerp && req.models.size() != 1)
        return err(ErrorCode::InvalidArgument, "slerp merges exactly two models: the base and one --model");
    if (is_lora && req.models.size() != 1)
        return err(ErrorCode::InvalidArgument, "lora merges exactly one LoRA file into the base");
    if (req.models.size() > 16)
        return err(ErrorCode::InvalidArgument, "at most 16 models per merge");
    const std::size_t n_weights = (P.method == Method::Linear) ? req.models.size() + 1 : req.models.size();
    std::vector<double> w = P.weights;
    if (w.empty())
        w.assign(n_weights, 1.0);
    if (w.size() != n_weights)
        return err(ErrorCode::InvalidArgument, "expected " + std::to_string(n_weights) +
                                                   " weight(s) for method '" + to_string(P.method) +
                                                   "', got " + std::to_string(w.size()));
    for (double v : w) {
        if (!std::isfinite(v) || v <= 0.0)
            return err(ErrorCode::InvalidArgument, "weights must be finite and > 0");
        if (is_lora && v > 8.0)
            return err(ErrorCode::InvalidArgument, "lora strength must be <= 8");
    }
    if (P.method == Method::Slerp && !(P.t >= 0.0 && P.t <= 1.0))
        return err(ErrorCode::InvalidArgument, "slerp t must be in [0,1]");
    if (P.method == Method::Ties && !(P.density > 0.0 && P.density <= 1.0))
        return err(ErrorCode::InvalidArgument, "ties density must be in (0,1]");
    if (P.method == Method::Dare && !(P.drop_rate >= 0.0 && P.drop_rate < 1.0))
        return err(ErrorCode::InvalidArgument, "dare drop rate must be in [0,1)");
    if (is_task && (!std::isfinite(P.lambda) || P.lambda <= 0.0))
        return err(ErrorCode::InvalidArgument, "lambda must be finite and > 0");
    if (is_lora && (!std::isfinite(P.lora_alpha) || P.lora_alpha < 0.0))
        return err(ErrorCode::InvalidArgument, "lora alpha must be >= 0");
    if (req.chunk_elements < 1024 || req.chunk_elements > (64u << 20))
        return err(ErrorCode::InvalidArgument, "chunk_elements must be 1024..67108864");

    // ---- paths: every input through the approved-root guard, output additionally never on C: ----
    std::vector<fs::path> in_paths;
    for (const auto* p : [&] {
             std::vector<const fs::path*> v{&req.base};
             for (const auto& m : req.models) v.push_back(&m);
             return v;
         }()) {
        auto r = guard.resolve(*p);
        if (!r)
            return r.error();
        std::error_code ec;
        if (!fs::is_regular_file(r.value(), ec))
            return err(ErrorCode::NotFound, "input '" + p->filename().string() + "' does not exist");
        in_paths.push_back(r.value());
    }
    auto outr = validate_merge_output(req.out, guard);
    if (!outr)
        return outr.error();
    const fs::path out_path = outr.value();
    for (const auto& ip : in_paths) {
        std::error_code ec;
        if (fs::exists(out_path, ec) && fs::equivalent(ip, out_path, ec))
            return err(ErrorCode::InvalidArgument, "the output must not be one of the inputs");
    }
    {
        std::error_code ec;
        if (fs::exists(out_path, ec) && !req.overwrite)
            return err(ErrorCode::AlreadyExists,
                       "'" + out_path.filename().string() + "' exists; pass overwrite to replace it");
    }

    // ---- open and cross-validate the inputs ----
    std::vector<std::unique_ptr<TensorFile>> files;
    for (const auto& ip : in_paths) {
        auto f = TensorFile::open(ip);
        if (!f)
            return err(f.error().code, "'" + ip.filename().string() + "': " + f.error().message);
        files.push_back(std::move(f.value()));
    }
    const TensorFile& base = *files[0];
    for (std::size_t i = 1; i < files.size(); ++i)
        if (files[i]->format() != base.format())
            return err(ErrorCode::InvalidArgument, std::string("all inputs must use one format: base is ") +
                                                       base.format_name() + ", input " + std::to_string(i) +
                                                       " is " + files[i]->format_name());

    std::vector<LoraPair> lora_pairs;
    std::map<std::string, const LoraPair*> lora_by_target;
    if (is_lora) {
        auto pr = discover_lora_pairs(*files[1], base);
        if (!pr)
            return pr.error();
        lora_pairs = std::move(pr.value());
        for (const auto& p : lora_pairs) lora_by_target[p.target] = &p;
    } else {
        for (std::size_t i = 1; i < files.size(); ++i) {
            const TensorFile& m = *files[i];
            const std::string who =
                "input " + std::to_string(i) + " ('" + m.path().filename().string() + "')";
            if (m.tensors().size() != base.tensors().size())
                return err(ErrorCode::InvalidArgument, who + " has " + std::to_string(m.tensors().size()) +
                                                           " tensors, the base has " +
                                                           std::to_string(base.tensors().size()));
            for (const auto& bt : base.tensors()) {
                const Tensor* mt = m.find(bt.name);
                if (!mt)
                    return err(ErrorCode::InvalidArgument, who + " has no tensor '" + bt.name + "'");
                if (!same_shape(bt, *mt))
                    return err(ErrorCode::InvalidArgument, who + ": tensor '" + bt.name + "' has shape " +
                                                               shape_str(mt->shape) + ", the base has " +
                                                               shape_str(bt.shape));
                if (detail::is_float(bt.dtype) != detail::is_float(mt->dtype) ||
                    (!detail::is_float(bt.dtype) && bt.type_name != mt->type_name))
                    return err(ErrorCode::InvalidArgument, who + ": tensor '" + bt.name + "' has type " +
                                                               mt->type_name + ", the base has " +
                                                               bt.type_name);
            }
        }
        if (base.format() == TensorFile::Format::Gguf)
            for (const auto& bt : base.tensors())
                if (!detail::is_float(bt.dtype))
                    return err(ErrorCode::Unsupported, "tensor '" + bt.name + "' is " + bt.type_name +
                                                           ": quantized GGUF models cannot be merged in "
                                                           "V1.1 (only F32/F16/BF16 tensors)");
    }

    // ---- memory accounting (fail closed before writing anything) ----
    const std::uint64_t chunk = req.chunk_elements;
    const std::uint64_t n_inputs = files.size();
    std::uint64_t peak =
        (n_inputs + 2) * chunk * 4;  // input chunks + output chunk (+ encode buffer is <= 4 B/elem)
    std::uint64_t largest = 0;
    for (const auto& t : base.tensors()) largest = std::max(largest, t.elements);
    if (P.method == Method::Ties)
        peak += largest * 4;  // |delta| selection buffer
    if (is_lora)
        for (const auto& p : lora_pairs)
            peak = std::max<std::uint64_t>(peak, (n_inputs + 2) * chunk * 4 + p.a->elements * 4);
    if (peak > req.max_ram_bytes)
        return err(ErrorCode::OutOfMemory,
                   "this merge needs about " + std::to_string(peak >> 20) + " MiB of working memory (" +
                       (P.method == Method::Ties ? "TIES keeps one whole tensor per selection; " : "") +
                       "largest tensor " + std::to_string(largest) + " elements) but the limit is " +
                       std::to_string(req.max_ram_bytes >> 20) + " MiB");

    // ---- write to <out>.partial ----
    std::error_code mkec;
    fs::create_directories(out_path.parent_path(), mkec);
    const fs::path partial = fs::path(out_path.string() + ".partial");
    // Declared BEFORE the sink: locals are destroyed in reverse order, so the sink's file handle is closed
    // before the cleanup tries to delete the partial file (Windows refuses to delete an open file).
    struct Cleanup {
        fs::path p;
        bool keep = false;
        ~Cleanup() {
            if (!keep) {
                std::error_code ec;
                fs::remove(p, ec);
            }
        }
    } cleanup{partial};
    Sink sink;
    if (auto r = sink.open(partial); !r)
        return r.error();

    Report rep;
    rep.format = base.format_name();
    rep.method = to_string(P.method);
    rep.tensors_total = base.tensors().size();
    rep.peak_buffer_bytes = peak;

    // Header
    if (base.format() == TensorFile::Format::Safetensors) {
        nlohmann::json h = nlohmann::json::object();
        nlohmann::json meta = nlohmann::json::object();
        for (const auto& [k, v] : base.metadata()) meta[k] = v;
        meta["hcs_merge_method"] = to_string(P.method);
        meta["hcs_merge_tool"] = "HCSInfer merge";
        h["__metadata__"] = meta;
        for (const auto& t : base.tensors())
            h[t.name] = {{"dtype", t.type_name},
                         {"shape", t.shape},
                         {"data_offsets", {t.rel_offset, t.rel_offset + t.bytes}}};
        std::string hs = h.dump();
        while (hs.size() % 8 != 0) hs.push_back(' ');
        std::uint8_t len[8];
        for (int i = 0; i < 8; ++i)
            len[i] = static_cast<std::uint8_t>((static_cast<std::uint64_t>(hs.size()) >> (8 * i)) & 0xff);
        if (auto r = sink.write(len, 8); !r)
            return r.error();
        if (auto r = sink.write(hs.data(), hs.size()); !r)
            return r.error();
    } else {
        std::vector<std::uint8_t> buf(1u << 20);
        for (std::uint64_t off = 0; off < base.data_offset();) {
            const std::size_t n =
                static_cast<std::size_t>(std::min<std::uint64_t>(buf.size(), base.data_offset() - off));
            if (auto r = base.read_bytes(off, buf.data(), n); !r)
                return r.error();
            if (auto r = sink.write(buf.data(), n); !r)
                return r.error();
            off += n;
        }
    }
    const std::uint64_t data_start = sink.written();

    // ---- tensors ----
    std::vector<std::vector<float>> in_buf(n_inputs);
    std::vector<float> out_buf;
    std::vector<std::uint8_t> enc;
    std::vector<float> scratch;
    std::vector<std::uint8_t> rawbuf(1u << 20);
    std::uint64_t total_bytes = 0, done_bytes = 0;
    for (const auto& t : base.tensors()) total_bytes += t.bytes;
    std::uint64_t cursor = 0;  // relative to the data section

    auto cancelled = [&]() { return req.token.cancelled(); };
    auto check_cancel = [&]() -> Result<void> {
        if (cancelled())
            return err(ErrorCode::Cancelled, "the merge was cancelled; no output was written");
        return {};
    };
    auto copy_range = [&](std::uint64_t file_off, std::uint64_t n) -> Result<void> {
        for (std::uint64_t done = 0; done < n;) {
            if (auto c = check_cancel(); !c)
                return c;
            const std::size_t k = static_cast<std::size_t>(std::min<std::uint64_t>(rawbuf.size(), n - done));
            if (auto r = base.read_bytes(file_off + done, rawbuf.data(), k); !r)
                return r;
            if (auto r = sink.write(rawbuf.data(), k); !r)
                return r;
            done += k;
        }
        return {};
    };
    auto read_into = [&](std::size_t i, const Tensor& t, std::uint64_t first,
                         std::uint64_t n) -> Result<void> {
        in_buf[i].resize(static_cast<std::size_t>(n));
        return files[i]->read_floats(t, first, n, in_buf[i].data());
    };
    auto emit = [&](const float* data, std::size_t n, DType d) -> Result<void> {
        encode(data, n, d, enc);
        return sink.write(enc.data(), enc.size());
    };

    for (const auto& bt : base.tensors()) {
        if (auto c = check_cancel(); !c)
            return c.error();
        // Gaps between tensors (alignment padding) are preserved byte for byte.
        if (bt.rel_offset > cursor)
            if (auto r = copy_range(base.data_offset() + cursor, bt.rel_offset - cursor); !r)
                return r.error();
        // cursor advances to the tensor END at the bottom of this loop; the value at its start is only
        // needed for the gap check above, so no intermediate assignment belongs here.
        if (req.progress)
            req.progress(
                total_bytes ? static_cast<double>(done_bytes) / static_cast<double>(total_bytes) : 0.0,
                bt.name);

        const LoraPair* lp = nullptr;
        if (is_lora) {
            if (auto it = lora_by_target.find(bt.name); it != lora_by_target.end())
                lp = it->second;
        }
        const bool merge_it = detail::is_float(bt.dtype) && (!is_lora || lp != nullptr);
        if (!merge_it) {
            if (auto r = copy_range(bt.file_offset, bt.bytes); !r)
                return r.error();
            ++rep.tensors_copied;
        } else if (P.method == Method::Linear) {
            for (std::uint64_t first = 0; first < bt.elements; first += chunk) {
                if (auto c = check_cancel(); !c)
                    return c.error();
                const std::uint64_t n = std::min<std::uint64_t>(chunk, bt.elements - first);
                std::vector<const float*> ptrs;
                for (std::size_t i = 0; i < files.size(); ++i) {
                    const Tensor* ti = i == 0 ? &bt : files[i]->find(bt.name);
                    if (auto r = read_into(i, *ti, first, n); !r)
                        return r.error();
                    ptrs.push_back(in_buf[i].data());
                }
                out_buf.resize(static_cast<std::size_t>(n));
                linear_chunk(ptrs, w, static_cast<std::size_t>(n), out_buf.data());
                if (auto r = emit(out_buf.data(), static_cast<std::size_t>(n), bt.dtype); !r)
                    return r.error();
            }
            ++rep.tensors_merged;
        } else if (P.method == Method::Slerp) {
            const Tensor& mt = *files[1]->find(bt.name);
            SlerpStats st;
            for (std::uint64_t first = 0; first < bt.elements; first += chunk) {
                if (auto c = check_cancel(); !c)
                    return c.error();
                const std::uint64_t n = std::min<std::uint64_t>(chunk, bt.elements - first);
                if (auto r = read_into(0, bt, first, n); !r)
                    return r.error();
                if (auto r = read_into(1, mt, first, n); !r)
                    return r.error();
                slerp_accumulate(in_buf[0].data(), in_buf[1].data(), static_cast<std::size_t>(n), st);
            }
            const SlerpCoeffs co = slerp_coeffs(st, P.t);
            for (std::uint64_t first = 0; first < bt.elements; first += chunk) {
                if (auto c = check_cancel(); !c)
                    return c.error();
                const std::uint64_t n = std::min<std::uint64_t>(chunk, bt.elements - first);
                if (auto r = read_into(0, bt, first, n); !r)
                    return r.error();
                if (auto r = read_into(1, mt, first, n); !r)
                    return r.error();
                out_buf.resize(static_cast<std::size_t>(n));
                slerp_apply(in_buf[0].data(), in_buf[1].data(), static_cast<std::size_t>(n), co,
                            out_buf.data());
                if (auto r = emit(out_buf.data(), static_cast<std::size_t>(n), bt.dtype); !r)
                    return r.error();
            }
            ++rep.tensors_merged;
        } else if (P.method == Method::Ties || P.method == Method::Dare) {
            const std::size_t m = files.size() - 1;
            std::vector<float> thr(m, 0.0f);
            if (P.method == Method::Ties) {
                for (std::size_t k = 0; k < m; ++k) {
                    const Tensor& mt = *files[k + 1]->find(bt.name);
                    scratch.resize(static_cast<std::size_t>(bt.elements));
                    for (std::uint64_t first = 0; first < bt.elements; first += chunk) {
                        if (auto c = check_cancel(); !c)
                            return c.error();
                        const std::uint64_t n = std::min<std::uint64_t>(chunk, bt.elements - first);
                        if (auto r = read_into(0, bt, first, n); !r)
                            return r.error();
                        if (auto r = read_into(k + 1, mt, first, n); !r)
                            return r.error();
                        for (std::uint64_t i = 0; i < n; ++i)
                            scratch[static_cast<std::size_t>(first + i)] = static_cast<float>(std::fabs(
                                static_cast<double>(in_buf[k + 1][i]) - static_cast<double>(in_buf[0][i])));
                    }
                    thr[k] = ties_select(scratch, P.density);
                }
            }
            const std::uint64_t key = fnv1a64(bt.name);
            for (std::uint64_t first = 0; first < bt.elements; first += chunk) {
                if (auto c = check_cancel(); !c)
                    return c.error();
                const std::uint64_t n = std::min<std::uint64_t>(chunk, bt.elements - first);
                if (auto r = read_into(0, bt, first, n); !r)
                    return r.error();
                std::vector<const float*> ptrs;
                for (std::size_t k = 0; k < m; ++k) {
                    if (auto r = read_into(k + 1, *files[k + 1]->find(bt.name), first, n); !r)
                        return r.error();
                    ptrs.push_back(in_buf[k + 1].data());
                }
                out_buf.resize(static_cast<std::size_t>(n));
                if (P.method == Method::Ties)
                    ties_chunk(in_buf[0].data(), ptrs, w, thr, P.lambda, static_cast<std::size_t>(n),
                               out_buf.data());
                else
                    dare_chunk(in_buf[0].data(), ptrs, w, P.drop_rate, P.seed, key, first, P.lambda,
                               static_cast<std::size_t>(n), out_buf.data());
                if (auto r = emit(out_buf.data(), static_cast<std::size_t>(n), bt.dtype); !r)
                    return r.error();
            }
            ++rep.tensors_merged;
        } else {  // LoRA into base
            const LoraPair& p = *lp;
            const TensorFile& lf = *files[1];
            const std::uint64_t rank = p.a->shape[0], cols = bt.shape[1], rows = bt.shape[0];
            std::vector<float> a_mat(static_cast<std::size_t>(rank * cols));
            if (auto r = lf.read_floats(*p.a, 0, rank * cols, a_mat.data()); !r)
                return r.error();
            const double alpha =
                p.alpha ? *p.alpha : (P.lora_alpha > 0.0 ? P.lora_alpha : static_cast<double>(rank));
            const double scale = w[0] * alpha / static_cast<double>(rank);
            const std::uint64_t rows_per = std::max<std::uint64_t>(1, chunk / cols);
            for (std::uint64_t r0 = 0; r0 < rows; r0 += rows_per) {
                if (auto c = check_cancel(); !c)
                    return c.error();
                const std::uint64_t nr = std::min<std::uint64_t>(rows_per, rows - r0);
                if (auto r = read_into(0, bt, r0 * cols, nr * cols); !r)
                    return r.error();
                in_buf[1].resize(static_cast<std::size_t>(nr * rank));
                if (auto r = lf.read_floats(*p.b, r0 * rank, nr * rank, in_buf[1].data()); !r)
                    return r.error();
                out_buf.resize(static_cast<std::size_t>(nr * cols));
                lora_rows(in_buf[0].data(), in_buf[1].data(), a_mat.data(), static_cast<std::size_t>(nr),
                          static_cast<std::size_t>(cols), static_cast<std::size_t>(rank), scale,
                          out_buf.data());
                if (auto r = emit(out_buf.data(), out_buf.size(), bt.dtype); !r)
                    return r.error();
            }
            ++rep.tensors_merged;
        }
        done_bytes += bt.bytes;
        cursor = bt.rel_offset + bt.bytes;
    }
    // Trailing bytes after the last tensor (GGUF only: the file may carry padding) are preserved.
    if (base.format() == TensorFile::Format::Gguf) {
        const std::uint64_t end = base.data_offset() + cursor;
        if (base.file_size() > end)
            if (auto r = copy_range(end, base.file_size() - end); !r)
                return r.error();
    }
    if (is_lora && rep.tensors_merged != lora_pairs.size())
        return err(ErrorCode::Internal, "internal error: not every LoRA pair was applied");
    if (auto r = sink.close(); !r)
        return r.error();
    rep.bytes_written = sink.written();
    rep.output_sha256 = sink.hex();
    if (base.format() == TensorFile::Format::Gguf && rep.bytes_written != base.file_size())
        return err(ErrorCode::Internal, "internal error: GGUF output size differs from the base");
    if (base.format() == TensorFile::Format::Safetensors && rep.bytes_written != data_start + cursor)
        return err(ErrorCode::Internal, "internal error: safetensors output size is inconsistent");

    // Re-open what was written and compare its structure with the base: names, shapes and dtypes.
    {
        auto chk = TensorFile::open(partial);
        if (!chk)
            return err(ErrorCode::Internal, "the merged file failed re-validation: " + chk.error().message);
        if (chk.value()->tensors().size() != base.tensors().size())
            return err(ErrorCode::Internal, "the merged file has a different tensor count than the base");
        for (const auto& bt : base.tensors()) {
            const Tensor* ot = chk.value()->find(bt.name);
            if (!ot || ot->shape != bt.shape || ot->type_name != bt.type_name)
                return err(ErrorCode::Internal,
                           "the merged file does not match the base structure at '" + bt.name + "'");
        }
    }

    {
        std::error_code ec;
        fs::rename(partial, out_path, ec);
        if (ec)
            return err(ErrorCode::IoError, "could not move the finished file into place: " + ec.message());
        cleanup.keep = true;
    }
    rep.output = out_path;
    if (req.write_manifest) {
        nlohmann::json j;
        j["schema_version"] = 1;
        j["tool"] = "HCSInfer merge";
        j["method"] = to_string(P.method);
        j["format"] = rep.format;
        j["params"] = {{"weights", w},
                       {"t", P.t},
                       {"density", P.density},
                       {"lambda", P.lambda},
                       {"drop_rate", P.drop_rate},
                       {"seed", P.seed},
                       {"lora_alpha", P.lora_alpha}};
        j["inputs"] = nlohmann::json::array();
        for (std::size_t i = 0; i < in_paths.size(); ++i)
            j["inputs"].push_back({{"role", i == 0 ? "base" : (is_lora ? "lora" : "model")},
                                   {"file", in_paths[i].filename().string()},
                                   {"size_bytes", files[i]->file_size()}});
        j["output"] = {{"file", out_path.filename().string()}, {"size_bytes", rep.bytes_written},
                       {"sha256", rep.output_sha256},          {"tensors", rep.tensors_total},
                       {"tensors_merged", rep.tensors_merged}, {"tensors_copied", rep.tensors_copied}};
        rep.manifest = fs::path(out_path.string() + ".merge.json");
        if (auto r = atomic_write_file(rep.manifest, j.dump(2) + "\n"); !r)
            return r.error();
    }
    rep.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return rep;
}

}  // namespace hcs::merge
