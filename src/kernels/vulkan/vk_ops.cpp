// HCS-owned Vulkan kernels (ported from hcsuda @94a3485, see docs/hcsuda-port.md) on top of the standalone
// runner. Host-side packing and dispatch geometry mirror hcsuda-cli/src/gpu.rs.
#include "hcs/kernels/vk_ops.hpp"

#include <algorithm>
#include <cstring>

#include "hcs/checked_math.hpp"
#include "hcs/shaders/GemmCoopMatF16.hpp"
#include "hcs/shaders/GemmF16Tiled.hpp"
#include "hcs/shaders/GemmF32Tiled.hpp"
#include "hcs/shaders/GemmI8DotTiled.hpp"
#include "hcs/shaders/LayerNormRows.hpp"
#include "hcs/shaders/RmsNormRows.hpp"
#include "hcs/shaders/RopeRows.hpp"
#include "hcs/shaders/SoftmaxRows.hpp"

namespace hcs::kernels::vulkan {

namespace {

struct Embedded {
    const char* id;
    const unsigned char* bytes;
    std::size_t size;
};

const std::vector<Embedded>& embedded() {
    static const std::vector<Embedded> e = {
        {"gemm_f32_tiled", shaders::kGemmF32Tiled, shaders::kGemmF32TiledSize},
        {"gemm_f16_tiled", shaders::kGemmF16Tiled, shaders::kGemmF16TiledSize},
        {"gemm_i8dot_tiled", shaders::kGemmI8DotTiled, shaders::kGemmI8DotTiledSize},
        {"gemm_coopmat_f16", shaders::kGemmCoopMatF16, shaders::kGemmCoopMatF16Size},
        {"softmax_rows", shaders::kSoftmaxRows, shaders::kSoftmaxRowsSize},
        {"layernorm_rows", shaders::kLayerNormRows, shaders::kLayerNormRowsSize},
        {"rmsnorm_rows", shaders::kRmsNormRows, shaders::kRmsNormRowsSize},
        {"rope_rows", shaders::kRopeRows, shaders::kRopeRowsSize},
    };
    return e;
}

const Embedded* find_embedded(const std::string& id) {
    for (const auto& e : embedded())
        if (id == e.id)
            return &e;
    return nullptr;
}

constexpr std::uint32_t kMaxGroups = 65535;  // portable minimum of maxComputeWorkGroupCount

std::optional<std::size_t> mul(std::size_t a, std::size_t b) {
    const auto r = checked_mul(a, b);
    if (!r || *r > static_cast<std::uint64_t>(~std::size_t{0}))
        return std::nullopt;
    return static_cast<std::size_t>(*r);
}

std::uint32_t ceil_div(std::uint32_t a, std::uint32_t b) {
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(a) + b - 1) / b);
}

struct Gemm3 {
    std::uint32_t m, n, k;
};
struct RowPush {
    std::uint32_t cols, rows;
};
struct NormPush {
    std::uint32_t cols, rows;
    float eps;
};
struct RopePush {
    std::uint32_t dim, rows, pos0;
    float base;
};

template <class T>
std::vector<unsigned char> bytes_of(const T& v) {
    std::vector<unsigned char> b(sizeof(T));
    std::memcpy(b.data(), &v, sizeof(T));
    return b;
}

}  // namespace

hardware::KernelHashes kernel_hashes() {
    hardware::KernelHashes h;
    for (const auto& e : embedded()) h[e.id] = hardware::fnv1a64_hex(e.bytes, e.size);
    return h;
}

Result<DispatchTiming> PreparedOp::run() {
    if (runner_ == nullptr || pipeline_ == nullptr)
        return make_error(ErrorCode::InvalidState, "PreparedOp is empty");
    std::vector<const Buffer*> ptrs;
    ptrs.reserve(buffers_.size());
    for (const auto& b : buffers_) ptrs.push_back(&b);
    return runner_->dispatch(*pipeline_, std::span<const Buffer* const>(ptrs.data(), ptrs.size()),
                             push_.empty() ? nullptr : push_.data(), static_cast<std::uint32_t>(push_.size()),
                             gx_, gy_, gz_);
}

Result<void> PreparedOp::read_output(void* dst, std::size_t bytes) {
    if (runner_ == nullptr || buffers_.empty())
        return make_error(ErrorCode::InvalidState, "PreparedOp is empty");
    if (bytes > output_bytes_)
        return make_error(ErrorCode::InvalidArgument, "read larger than the output");
    return runner_->download(buffers_.back(), dst, bytes);
}

hardware::Gate KernelSet::gate(const std::string& id, const hardware::Shape* shape) const {
    const hardware::KernelDesc* k = hardware::find_kernel(id);
    if (k == nullptr)
        return {false, "unknown kernel id " + id};
    if (find_embedded(id) == nullptr)
        return {false, "kernel " + id + " is not embedded in this binary"};
    return hardware::gate_kernel(*k, runner_->caps(), shape);
}

Result<const Pipeline*> KernelSet::pipeline(const std::string& id, std::uint32_t num_buffers,
                                            std::uint32_t push_bytes) {
    if (auto it = pipelines_.find(id); it != pipelines_.end())
        return static_cast<const Pipeline*>(&it->second);
    const Embedded* e = find_embedded(id);
    if (e == nullptr)
        return make_error(ErrorCode::Unsupported, "kernel " + id + " is not embedded in this binary");
    auto p = runner_->create_pipeline(e->bytes, e->size, num_buffers, push_bytes);
    if (!p)
        return p.error();
    auto ins = pipelines_.emplace(id, std::move(p.value()));
    return static_cast<const Pipeline*>(&ins.first->second);
}

Result<PreparedOp> KernelSet::build(const std::string& id,
                                    std::vector<std::pair<const void*, std::size_t>> inputs,
                                    std::size_t output_bytes, const void* push, std::uint32_t push_bytes,
                                    std::uint32_t gx, std::uint32_t gy, double flops) {
    if (gx == 0 || gy == 0)
        return make_error(ErrorCode::InvalidArgument, "empty problem");
    if (gx > kMaxGroups || gy > kMaxGroups)
        return make_error(ErrorCode::Unsupported,
                          "problem needs more than 65535 workgroups in one dimension");
    const std::uint64_t limit = runner_->caps().max_storage_range.value_or(1ull << 27);
    for (const auto& in : inputs)
        if (in.second > limit)
            return make_error(ErrorCode::Unsupported, "input exceeds the device's max storage buffer range");
    if (output_bytes > limit)
        return make_error(ErrorCode::Unsupported, "output exceeds the device's max storage buffer range");

    const auto num_buffers = static_cast<std::uint32_t>(inputs.size() + 1);
    auto pipe = pipeline(id, num_buffers, push_bytes);
    if (!pipe)
        return pipe.error();
    PreparedOp op;
    op.runner_ = runner_;
    op.pipeline_ = pipe.value();
    op.id_ = id;
    for (const auto& in : inputs) {
        auto b = runner_->create_buffer(in.second);
        if (!b)
            return b.error();
        if (auto up = runner_->upload(b.value(), in.first, in.second); !up)
            return up.error();
        op.buffers_.push_back(std::move(b.value()));
    }
    auto out = runner_->create_buffer(output_bytes);
    if (!out)
        return out.error();
    op.buffers_.push_back(std::move(out.value()));
    op.push_.assign(static_cast<const unsigned char*>(push),
                    static_cast<const unsigned char*>(push) + push_bytes);
    op.gx_ = gx;
    op.gy_ = gy;
    op.output_bytes_ = output_bytes;
    op.flops_ = flops;
    return op;
}

namespace {
Error gate_error(const std::string& id, const hardware::Gate& g) {
    return make_error(ErrorCode::Unsupported, "kernel " + id + " is not usable on this device: " + g.reason);
}
Error bad(const std::string& why) {
    return make_error(ErrorCode::InvalidArgument, why);
}
}  // namespace

Result<PreparedOp> KernelSet::prepare_gemm_f32(const float* a, const float* b, std::uint32_t m,
                                               std::uint32_t n, std::uint32_t k) {
    if (a == nullptr || b == nullptr || m == 0 || n == 0 || k == 0)
        return bad("gemm_f32: null input or empty shape");
    const hardware::Shape sh{m, n, k};
    if (auto g = gate("gemm_f32_tiled", &sh); !g.eligible)
        return gate_error("gemm_f32_tiled", g);
    const auto ab = mul(mul(m, k).value_or(0), sizeof(float)), bb = mul(mul(k, n).value_or(0), sizeof(float)),
               cb = mul(mul(m, n).value_or(0), sizeof(float));
    if (!ab || !bb || !cb || *ab == 0 || *bb == 0 || *cb == 0)
        return bad("gemm_f32: shape overflows");
    const Gemm3 pc{m, n, k};
    return build("gemm_f32_tiled", {{a, *ab}, {b, *bb}}, *cb, &pc, sizeof(pc), ceil_div(n, 16),
                 ceil_div(m, 16), 2.0 * m * n * k);
}

Result<PreparedOp> KernelSet::prepare_gemm_f16(const std::uint16_t* a, const std::uint16_t* b,
                                               std::uint32_t m, std::uint32_t n, std::uint32_t k) {
    if (a == nullptr || b == nullptr || m == 0 || n == 0 || k == 0)
        return bad("gemm_f16: null input or empty shape");
    const hardware::Shape sh{m, n, k};
    if (auto g = gate("gemm_f16_tiled", &sh); !g.eligible)
        return gate_error("gemm_f16_tiled", g);
    const auto na = mul(m, k), nb = mul(k, n), nc = mul(mul(m, n).value_or(0), sizeof(float));
    if (!na || !nb || !nc || *na == 0 || *nb == 0 || *nc == 0)
        return bad("gemm_f16: shape overflows");
    // The shader reads two halves per uint: pad odd element counts to an even number.
    std::vector<std::uint16_t> pa(a, a + *na), pb(b, b + *nb);
    if (pa.size() % 2)
        pa.push_back(0);
    if (pb.size() % 2)
        pb.push_back(0);
    const Gemm3 pc{m, n, k};
    return build("gemm_f16_tiled", {{pa.data(), pa.size() * 2}, {pb.data(), pb.size() * 2}}, *nc, &pc,
                 sizeof(pc), ceil_div(n, 16), ceil_div(m, 16), 2.0 * m * n * k);
}

Result<PreparedOp> KernelSet::prepare_gemm_i8dot(const std::int8_t* a, const std::int8_t* b, std::uint32_t m,
                                                 std::uint32_t n, std::uint32_t k) {
    if (a == nullptr || b == nullptr || m == 0 || n == 0 || k == 0)
        return bad("gemm_i8dot: null input or empty shape");
    const hardware::Shape sh{m, n, k};
    if (auto g = gate("gemm_i8dot_tiled", &sh); !g.eligible)
        return gate_error("gemm_i8dot_tiled", g);
    const auto na = mul(m, k), nb = mul(k, n), nc = mul(mul(m, n).value_or(0), sizeof(std::int32_t));
    if (!na || !nb || !nc || *na == 0 || *nb == 0 || *nc == 0)
        return bad("gemm_i8dot: shape overflows");
    // B is supplied transposed (n x k) so both operands of every dot product are contiguous words.
    std::vector<std::int8_t> bt(*nb);
    for (std::size_t r = 0; r < k; ++r)
        for (std::size_t c = 0; c < n; ++c) bt[c * k + r] = b[r * n + c];
    const Gemm3 pc{m, n, k};
    return build("gemm_i8dot_tiled", {{a, *na}, {bt.data(), bt.size()}}, *nc, &pc, sizeof(pc),
                 ceil_div(n, 16), ceil_div(m, 16), 2.0 * m * n * k);
}

Result<PreparedOp> KernelSet::prepare_gemm_coopmat_f16(const std::uint16_t* a, const std::uint16_t* b,
                                                       std::uint32_t m, std::uint32_t n, std::uint32_t k) {
    if (a == nullptr || b == nullptr || m == 0 || n == 0 || k == 0)
        return bad("gemm_coopmat: null input or empty shape");
    const hardware::Shape sh{m, n, k};
    if (auto g = gate("gemm_coopmat_f16", &sh); !g.eligible)
        return gate_error("gemm_coopmat_f16", g);
    const auto na = mul(mul(m, k).value_or(0), 2), nb = mul(mul(k, n).value_or(0), 2),
               nc = mul(mul(m, n).value_or(0), sizeof(float));
    if (!na || !nb || !nc || *na == 0 || *nb == 0 || *nc == 0)
        return bad("gemm_coopmat: shape overflows");
    const Gemm3 pc{m, n, k};
    return build("gemm_coopmat_f16", {{a, *na}, {b, *nb}}, *nc, &pc, sizeof(pc), n / 16, m / 16,
                 2.0 * m * n * k);
}

Result<PreparedOp> KernelSet::prepare_softmax(const float* x, std::uint32_t rows, std::uint32_t cols) {
    if (x == nullptr || rows == 0 || cols == 0)
        return bad("softmax: null input or empty shape");
    const hardware::Shape sh{rows, cols, 0};
    if (auto g = gate("softmax_rows", &sh); !g.eligible)
        return gate_error("softmax_rows", g);
    const auto nb = mul(mul(rows, cols).value_or(0), sizeof(float));
    if (!nb || *nb == 0)
        return bad("softmax: shape overflows");
    const RowPush pc{cols, rows};
    // ~5 flops per element (max, exp, sum, scale) - nominal, used only for a rate display.
    return build("softmax_rows", {{x, *nb}}, *nb, &pc, sizeof(pc), rows, 1, 5.0 * rows * cols);
}

Result<PreparedOp> KernelSet::prepare_layernorm(const float* x, const float* gamma, const float* beta,
                                                std::uint32_t rows, std::uint32_t cols, float eps) {
    if (x == nullptr || gamma == nullptr || beta == nullptr || rows == 0 || cols == 0)
        return bad("layernorm: null input or empty shape");
    const hardware::Shape sh{rows, cols, 0};
    if (auto g = gate("layernorm_rows", &sh); !g.eligible)
        return gate_error("layernorm_rows", g);
    const auto nb = mul(mul(rows, cols).value_or(0), sizeof(float));
    if (!nb || *nb == 0)
        return bad("layernorm: shape overflows");
    const std::size_t gb = static_cast<std::size_t>(cols) * sizeof(float);
    const NormPush pc{cols, rows, eps};
    return build("layernorm_rows", {{x, *nb}, {gamma, gb}, {beta, gb}}, *nb, &pc, sizeof(pc), rows, 1,
                 7.0 * rows * cols);
}

Result<PreparedOp> KernelSet::prepare_rmsnorm(const float* x, const float* gamma, std::uint32_t rows,
                                              std::uint32_t cols, float eps) {
    if (x == nullptr || gamma == nullptr || rows == 0 || cols == 0)
        return bad("rmsnorm: null input or empty shape");
    const hardware::Shape sh{rows, cols, 0};
    if (auto g = gate("rmsnorm_rows", &sh); !g.eligible)
        return gate_error("rmsnorm_rows", g);
    const auto nb = mul(mul(rows, cols).value_or(0), sizeof(float));
    if (!nb || *nb == 0)
        return bad("rmsnorm: shape overflows");
    const std::size_t gb = static_cast<std::size_t>(cols) * sizeof(float);
    const NormPush pc{cols, rows, eps};
    return build("rmsnorm_rows", {{x, *nb}, {gamma, gb}}, *nb, &pc, sizeof(pc), rows, 1, 4.0 * rows * cols);
}

Result<PreparedOp> KernelSet::prepare_rope(const float* x, std::uint32_t rows, std::uint32_t dim,
                                           std::uint32_t pos0, float base) {
    if (x == nullptr || rows == 0 || dim == 0)
        return bad("rope: null input or empty shape");
    if (dim % 2 != 0)
        return bad("rope: dim must be even");
    const hardware::Shape sh{rows, dim, 0};
    if (auto g = gate("rope_rows", &sh); !g.eligible)
        return gate_error("rope_rows", g);
    const auto nb = mul(mul(rows, dim).value_or(0), sizeof(float));
    const auto pairs = mul(rows, dim / 2);
    if (!nb || !pairs || *nb == 0)
        return bad("rope: shape overflows");
    const RopePush pc{dim, rows, pos0, base};
    const std::uint64_t groups = (*pairs + 255) / 256;
    if (groups > kMaxGroups)
        return make_error(ErrorCode::Unsupported, "rope: problem needs more than 65535 workgroups");
    return build("rope_rows", {{x, *nb}}, *nb, &pc, sizeof(pc), static_cast<std::uint32_t>(groups), 1,
                 6.0 * static_cast<double>(*pairs));
}

}  // namespace hcs::kernels::vulkan
