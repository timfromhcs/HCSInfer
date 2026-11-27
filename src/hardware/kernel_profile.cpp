// Kernel profile cache and the promotion rule. Pure logic + JSON (no Vulkan).
#include "hcs/hardware/kernel_profile.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "hcs/fs_util.hpp"
#include "hw.hpp"

namespace hcs::hardware {

namespace {
constexpr std::uint64_t kMaxProfileBytes = 8ull << 20;

bool same_key(const ProfileEntry& e, const std::string& dev, const std::string& kid, const std::string& hash,
              const std::string& bucket) {
    return e.device_key == dev && e.kernel_id == kid && e.kernel_hash == hash && e.shape_bucket == bucket;
}

json entry_to_json(const ProfileEntry& e) {
    json j;
    j["device_key"] = e.device_key;
    j["kernel_id"] = e.kernel_id;
    j["kernel_hash"] = e.kernel_hash;
    j["shape_bucket"] = e.shape_bucket;
    j["device_name"] = e.device_name;
    j["driver_info"] = e.driver_info;
    j["shape"] = {{"m", e.m}, {"n", e.n}, {"k", e.k}};
    j["timestamp_utc"] = e.timestamp_utc;
    j["hcs_commit"] = e.hcs_commit;
    j["correctness"] = {{"correct", e.correct},
                        {"max_abs_error", e.max_abs_error},
                        {"tolerance", {{"rel", e.tol_rel}, {"abs", e.tol_abs}}},
                        {"identical_runs", e.identical_runs},
                        {"total_runs", e.total_runs}};
    json t = {{"samples", e.samples},
              {"wall_ms_median", e.wall_ms_median},
              {"wall_spread", e.wall_spread},
              {"gpu_ms_median", e.gpu_ms_median ? json(*e.gpu_ms_median) : json(nullptr)},
              {"gpu_spread", e.gpu_spread ? json(*e.gpu_spread) : json(nullptr)},
              {"flops", e.flops}};
    j["timing"] = t;
    j["baseline"] = {{"name", e.baseline_name}, {"ms", e.baseline_ms}};
    j["upstream_ggml_vulkan"] = {{"status", e.upstream_status},
                                 {"ratio", e.upstream_ratio ? json(*e.upstream_ratio) : json(nullptr)},
                                 {"median_ms", e.upstream_ms},
                                 {"spread", e.upstream_spread},
                                 {"variant", e.upstream_variant},
                                 {"note", e.upstream_note}};
    j["speedup_vs_baseline"] = e.speedup();
    return j;
}

std::optional<ProfileEntry> entry_from_json(const json& j) {
    if (!j.is_object())
        return std::nullopt;
    try {
        ProfileEntry e;
        e.device_key = j.at("device_key").get<std::string>();
        e.kernel_id = j.at("kernel_id").get<std::string>();
        e.kernel_hash = j.at("kernel_hash").get<std::string>();
        e.shape_bucket = j.at("shape_bucket").get<std::string>();
        e.device_name = j.value("device_name", "");
        e.driver_info = j.value("driver_info", "");
        e.timestamp_utc = j.value("timestamp_utc", "");
        e.hcs_commit = j.value("hcs_commit", "");
        if (j.contains("shape")) {
            e.m = j["shape"].value("m", std::uint64_t{0});
            e.n = j["shape"].value("n", std::uint64_t{0});
            e.k = j["shape"].value("k", std::uint64_t{0});
        }
        const json& c = j.at("correctness");
        e.correct = c.at("correct").get<bool>();
        e.max_abs_error = c.at("max_abs_error").get<double>();
        e.tol_rel = c["tolerance"].value("rel", 0.0);
        e.tol_abs = c["tolerance"].value("abs", 0.0);
        e.identical_runs = c.at("identical_runs").get<std::uint32_t>();
        e.total_runs = c.at("total_runs").get<std::uint32_t>();
        const json& t = j.at("timing");
        e.samples = t.at("samples").get<std::uint32_t>();
        e.wall_ms_median = t.at("wall_ms_median").get<double>();
        e.wall_spread = t.at("wall_spread").get<double>();
        if (t.contains("gpu_ms_median") && t["gpu_ms_median"].is_number())
            e.gpu_ms_median = t["gpu_ms_median"].get<double>();
        if (t.contains("gpu_spread") && t["gpu_spread"].is_number())
            e.gpu_spread = t["gpu_spread"].get<double>();
        e.flops = t.value("flops", 0.0);
        e.baseline_name = j.at("baseline").at("name").get<std::string>();
        e.baseline_ms = j.at("baseline").at("ms").get<double>();
        const json& u = j.at("upstream_ggml_vulkan");
        e.upstream_status = u.at("status").get<std::string>();
        if (u.contains("ratio") && u["ratio"].is_number())
            e.upstream_ratio = u["ratio"].get<double>();
        e.upstream_ms = u.value("median_ms", 0.0);  // absent in profiles written before wave C / k1
        e.upstream_spread = u.value("spread", 0.0);
        e.upstream_variant = u.value("variant", "");
        e.upstream_note = u.value("note", "");
        return e;
    } catch (const json::exception&) {
        return std::nullopt;
    }
}
}  // namespace

double ProfileEntry::speedup() const {
    if (!(baseline_ms > 0.0) || !(wall_ms_median > 0.0))
        return 0.0;
    return baseline_ms / wall_ms_median;
}

std::optional<double> ProfileEntry::gflops_wall() const {
    if (!(flops > 0.0) || !(wall_ms_median > 0.0))
        return std::nullopt;
    return flops / (wall_ms_median * 1e-3) / 1e9;
}

std::optional<double> ProfileEntry::gflops_gpu() const {
    if (!(flops > 0.0) || !gpu_ms_median || !(*gpu_ms_median > 0.0))
        return std::nullopt;
    return flops / (*gpu_ms_median * 1e-3) / 1e9;
}

PromotionDecision evaluate_promotion(const ProfileEntry& e, const PromotionPolicy& p) {
    PromotionDecision d;
    char buf[160];
    d.correct_ok = e.correct && std::isfinite(e.max_abs_error);
    if (!d.correct_ok)
        d.reasons.push_back("not correct vs the CPU oracle");

    d.determinism_ok = e.total_runs >= p.min_identical_runs && e.identical_runs == e.total_runs;
    if (!d.determinism_ok) {
        std::snprintf(buf, sizeof(buf), "needs %u bit-identical runs (got %u of %u identical)",
                      p.min_identical_runs, e.identical_runs, e.total_runs);
        d.reasons.push_back(buf);
    }

    d.noise_ok = std::isfinite(e.wall_spread) && e.wall_spread <= p.max_spread;
    if (!d.noise_ok) {
        std::snprintf(buf, sizeof(buf), "timing too noisy (spread %.3f > %.3f): not a result", e.wall_spread,
                      p.max_spread);
        d.reasons.push_back(buf);
    }

    const double sp = e.speedup();
    d.speed_ok = sp > 0.0 && sp + 1e-12 >= p.min_speedup;
    if (!d.speed_ok) {
        if (sp <= 0.0)
            d.reasons.push_back("no baseline measured");
        else {
            std::snprintf(buf, sizeof(buf), "speedup %.3fx vs %s is below the required %.2fx", sp,
                          e.baseline_name.c_str(), p.min_speedup);
            d.reasons.push_back(buf);
        }
    }

    d.promoted = d.correct_ok && d.determinism_ok && d.noise_ok && d.speed_ok;
    const bool upstream_measured = e.upstream_status == "measured" && e.upstream_ratio.has_value();
    const bool upstream_noise_ok = std::isfinite(e.upstream_spread) && e.upstream_spread <= p.max_spread;
    const bool upstream_ok =
        upstream_measured && upstream_noise_ok && *e.upstream_ratio + 1e-12 >= p.min_speedup;
    d.default_enabled = d.promoted && upstream_ok;
    if (d.promoted && !upstream_ok) {
        if (e.upstream_status == "not_comparable")
            d.reasons.push_back(
                "no equivalent upstream GGML operator to compare against: promoted but not default-enabled");
        else if (e.upstream_status == "error")
            d.reasons.push_back(
                "upstream GGML Vulkan baseline failed to run: promoted but not default-enabled");
        else if (!upstream_measured)
            d.reasons.push_back("upstream GGML Vulkan baseline NOT RUN: promoted but not default-enabled");
        else if (!upstream_noise_ok) {
            std::snprintf(buf, sizeof(buf), "upstream timing too noisy (spread %.3f > %.3f): not a result",
                          e.upstream_spread, p.max_spread);
            d.reasons.push_back(buf);
        } else
            d.reasons.push_back("upstream GGML Vulkan baseline did not reach the required speedup");
    }
    return d;
}

Result<void> KernelProfile::upsert(ProfileEntry e) {
    if (!e.correct)
        return make_error(
            ErrorCode::InvalidArgument,
            "refusing to persist a profile entry that failed correctness (" + e.kernel_id + ")");
    if (e.device_key.empty() || e.kernel_id.empty() || e.kernel_hash.empty() || e.shape_bucket.empty())
        return make_error(ErrorCode::InvalidArgument, "profile entry has an incomplete key");
    for (auto& x : entries_) {
        if (same_key(x, e.device_key, e.kernel_id, e.kernel_hash, e.shape_bucket)) {
            x = std::move(e);
            return {};
        }
    }
    entries_.push_back(std::move(e));
    return {};
}

bool KernelProfile::remove(const std::string& dev, const std::string& kid, const std::string& hash,
                           const std::string& bucket) {
    const auto before = entries_.size();
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const ProfileEntry& e) { return same_key(e, dev, kid, hash, bucket); }),
                   entries_.end());
    return entries_.size() != before;
}

const ProfileEntry* KernelProfile::find(const std::string& dev, const std::string& kid,
                                        const std::string& hash, const std::string& bucket) const {
    for (const auto& e : entries_)
        if (same_key(e, dev, kid, hash, bucket))
            return &e;
    return nullptr;
}

bool KernelProfile::has_stale(const std::string& dev, const std::string& kid, const std::string& hash,
                              const std::string& bucket) const {
    for (const auto& e : entries_)
        if (e.kernel_id == kid && e.shape_bucket == bucket && !same_key(e, dev, kid, hash, bucket))
            return true;
    return false;
}

std::size_t KernelProfile::reset(const std::string& prefix, const std::string& kernel_id) {
    const auto before = entries_.size();
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const ProfileEntry& e) {
                                      if (e.device_key.compare(0, prefix.size(), prefix) != 0)
                                          return false;
                                      return kernel_id.empty() || e.kernel_id == kernel_id;
                                  }),
                   entries_.end());
    return before - entries_.size();
}

std::size_t KernelProfile::prune_stale(const std::string& live_device_key,
                                       const std::vector<std::pair<std::string, std::string>>& live) {
    const auto before = entries_.size();
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const ProfileEntry& e) {
                                      if (e.device_key != live_device_key)
                                          return false;  // other devices are not ours to judge
                                      for (const auto& [id, hash] : live)
                                          if (id == e.kernel_id)
                                              return hash != e.kernel_hash;
                                      return true;  // kernel no longer exists
                                  }),
                   entries_.end());
    return before - entries_.size();
}

std::string KernelProfile::to_json_text() const {
    json j;
    j["schema_version"] = kProfileSchemaVersion;
    json arr = json::array();
    for (const auto& e : entries_) arr.push_back(entry_to_json(e));
    j["entries"] = std::move(arr);
    return j.dump(2) + "\n";
}

Result<KernelProfile> KernelProfile::from_json_text(const std::string& text) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return make_error(ErrorCode::InvalidArgument, "kernel profile is not valid JSON");
    if (j.value("schema_version", 0) != kProfileSchemaVersion)
        return make_error(ErrorCode::InvalidArgument, "kernel profile has an unsupported schema_version");
    if (!j.contains("entries") || !j["entries"].is_array())
        return make_error(ErrorCode::InvalidArgument, "kernel profile has no entries array");
    KernelProfile p;
    for (const auto& ej : j["entries"]) {
        auto e = entry_from_json(ej);
        if (!e)
            return make_error(ErrorCode::InvalidArgument, "kernel profile contains a malformed entry");
        if (auto r = p.upsert(std::move(*e)); !r)
            return r.error();
    }
    return p;
}

std::filesystem::path profile_path(const std::filesystem::path& state_dir) {
    return state_dir / "kernel-profile.json";
}

Result<KernelProfile> load_profile(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return KernelProfile{};
    auto text = read_file_limited(path, kMaxProfileBytes);
    if (!text)
        return text.error();
    return KernelProfile::from_json_text(text.value());
}

Result<void> save_profile(const std::filesystem::path& path, const KernelProfile& profile) {
    return atomic_write_file(path, profile.to_json_text());
}

std::string fnv1a64_hex(const void* data, std::size_t size) {
    std::uint64_t h = 14695981039346656037ull;
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

}  // namespace hcs::hardware
