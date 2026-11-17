#include "hcs/train/adapter_book.hpp"

#include <cmath>
#include <cstdint>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "hcs/fs_util.hpp"
#include "train_util.hpp"

namespace hcs::train {

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr std::uint64_t kMaxManifestBytes = 1u << 20;
constexpr std::uint64_t kMaxPointerBytes = 1u << 20;

bool score_ok(double v) {
    return std::isfinite(v) && v >= 0.0 && v <= 1.0;
}

json scores_to_json(const EvalScores& s) {
    json regression = json::object();
    for (const auto& [name, score] : s.regression) {
        regression[name] = score;
    }
    return json{{"heldout", s.heldout}, {"heldout_samples", s.heldout_samples}, {"regression", regression}};
}

// Parses one scores object; returns false on any defect.
bool parse_scores(const json& j, EvalScores& out) {
    if (!j.is_object()) {
        return false;
    }
    const auto held = j.find("heldout");
    if (held == j.end() || !held->is_number() || !score_ok(held->get<double>())) {
        return false;
    }
    out.heldout = held->get<double>();
    const auto count = j.find("heldout_samples");
    if (count == j.end() || !count->is_number_unsigned()) {
        return false;
    }
    out.heldout_samples = count->get<std::size_t>();
    const auto reg = j.find("regression");
    if (reg == j.end() || !reg->is_object()) {
        return false;
    }
    out.regression.clear();
    for (const auto& [name, value] : reg->items()) {
        if (name.empty() || !value.is_number() || !score_ok(value.get<double>())) {
            return false;
        }
        out.regression[name] = value.get<double>();
    }
    return true;
}

GateDecision parse_decision(const std::string& s) {
    if (s == "keep") {
        return GateDecision::Keep;
    }
    if (s == "rollback") {
        return GateDecision::Rollback;
    }
    return GateDecision::Invalid;
}

json gate_to_json(const GateResult& g) {
    return json{{"decision", to_string(g.decision)},
                {"reason", g.reason},
                {"heldout_delta", g.heldout_delta},
                {"regressed_suites", g.regressed_suites}};
}

bool parse_gate(const json& j, GateResult& out) {
    if (!j.is_object()) {
        return false;
    }
    const auto decision = j.find("decision");
    if (decision == j.end() || !decision->is_string()) {
        return false;
    }
    const std::string d = decision->get<std::string>();
    if (d != "keep" && d != "rollback" && d != "invalid") {
        return false;
    }
    out.decision = parse_decision(d);
    const auto reason = j.find("reason");
    if (reason == j.end() || !reason->is_string()) {
        return false;
    }
    out.reason = reason->get<std::string>();
    const auto delta = j.find("heldout_delta");
    if (delta == j.end() || !delta->is_number() || !std::isfinite(delta->get<double>())) {
        return false;
    }
    out.heldout_delta = delta->get<double>();
    out.regressed_suites.clear();
    const auto regressed = j.find("regressed_suites");
    if (regressed == j.end() || !regressed->is_array()) {
        return false;
    }
    for (const auto& suite : *regressed) {
        if (!suite.is_string() || suite.get_ref<const std::string&>().empty()) {
            return false;
        }
        out.regressed_suites.push_back(suite.get<std::string>());
    }
    return true;
}

// Returns an empty string when the manifest is internally consistent.
std::string check_manifest(const AdapterManifest& m) {
    if (m.schema_version != 1) {
        return "manifest schema_version must be 1";
    }
    if (!detail::valid_adapter_id(m.adapter_id)) {
        return "adapter_id must be a safe identifier (max 64, [A-Za-z0-9._-])";
    }
    if (!detail::is_hex64(m.base_model_sha256)) {
        return "base_model_sha256 must be 64 hex characters";
    }
    if (!detail::is_hex64(m.adapter_sha256)) {
        return "adapter_sha256 must be 64 hex characters";
    }
    if (!detail::is_hex64(m.dataset_manifest_sha256)) {
        return "dataset_manifest_sha256 must be 64 hex characters";
    }
    if (m.artifact_path.empty() || m.artifact_path.size() > 4096) {
        return "artifact_path must be a non-empty path (max 4096 bytes)";
    }
    if (!detail::valid_eval_id(m.eval_id)) {
        return "eval_id must be a non-empty identifier (max 128)";
    }
    if (m.created_at.empty() || m.created_at.size() > 64) {
        return "created_at must be a non-empty timestamp string";
    }
    for (const auto& [key, value] : m.hyperparameters) {
        if (key.empty() || key.size() > 64 || value.size() > 256) {
            return "hyperparameters must be short string keys and values";
        }
    }
    if (m.gate.reason.empty()) {
        return "gate.reason must be recorded";
    }
    return {};
}

struct TargetEntry {
    std::string active;
    std::string previous;
};

using Pointer = std::map<std::string, TargetEntry>;  // key: lower-case base model sha256

Result<Pointer> read_pointer(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return Pointer{};
    }
    auto text = read_file_limited(path, kMaxPointerBytes);
    if (!text) {
        return text.error();
    }
    const json root = json::parse(text.value(), nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return make_error(ErrorCode::IoError, "adapter pointer file is not valid JSON");
    }
    const auto version = root.find("schema_version");
    if (version == root.end() || !version->is_number_integer() || version->get<int>() != 1) {
        return make_error(ErrorCode::IoError, "adapter pointer file has an unsupported schema_version");
    }
    Pointer out;
    const auto targets = root.find("targets");
    if (targets == root.end() || !targets->is_object()) {
        return make_error(ErrorCode::IoError, "adapter pointer file has no `targets` object");
    }
    for (const auto& [base, entry] : targets->items()) {
        if (!entry.is_object()) {
            return make_error(ErrorCode::IoError, "adapter pointer target '" + base + "' is not an object");
        }
        TargetEntry e;
        const auto active = entry.find("active");
        const auto previous = entry.find("previous");
        if (active == entry.end() || !active->is_string() || previous == entry.end() ||
            !previous->is_string()) {
            return make_error(ErrorCode::IoError, "adapter pointer target '" + base + "' is malformed");
        }
        e.active = active->get<std::string>();
        e.previous = previous->get<std::string>();
        out[detail::lower(base)] = std::move(e);
    }
    return out;
}

Result<void> write_pointer(const fs::path& path, const Pointer& pointer) {
    json targets = json::object();
    for (const auto& [base, entry] : pointer) {
        targets[base] = json{{"active", entry.active}, {"previous", entry.previous}};
    }
    const json root{{"schema_version", 1}, {"targets", std::move(targets)}};
    return atomic_write_file(path, root.dump(2));
}

Result<AdapterManifest> read_manifest_file(const fs::path& path) {
    auto text = read_file_limited(path, kMaxManifestBytes);
    if (!text) {
        return text.error();
    }
    return parse_adapter_manifest(text.value());
}

}  // namespace

std::string to_json(const AdapterManifest& m) {
    json hyper = json::object();
    for (const auto& [key, value] : m.hyperparameters) {
        hyper[key] = value;
    }
    json j{
        {"schema_version", m.schema_version},
        {"adapter_id", m.adapter_id},
        {"base_model_sha256", m.base_model_sha256},
        {"adapter_sha256", m.adapter_sha256},
        {"artifact_path", m.artifact_path},
        {"dataset_manifest_sha256", m.dataset_manifest_sha256},
        {"eval_id", m.eval_id},
        {"hyperparameters", std::move(hyper)},
        {"baseline", scores_to_json(m.baseline)},
        {"candidate", scores_to_json(m.candidate)},
        {"gate", gate_to_json(m.gate)},
        {"created_at", m.created_at},
    };
    return j.dump(2);
}

Result<AdapterManifest> parse_adapter_manifest(std::string_view text) {
    if (text.empty()) {
        return make_error(ErrorCode::InvalidArgument, "manifest is empty");
    }
    if (detail::json_depth(text) > detail::kMaxJsonDepth) {
        return make_error(ErrorCode::InvalidArgument, "manifest JSON nests too deeply");
    }
    const json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return make_error(ErrorCode::InvalidArgument, "manifest is not a JSON object");
    }
    const auto str = [&](const char* field) -> const json* {
        const auto it = root.find(field);
        return (it != root.end() && it->is_string()) ? &*it : nullptr;
    };
    AdapterManifest m;
    const auto version = root.find("schema_version");
    if (version == root.end() || !version->is_number_integer() || version->get<int>() != 1) {
        return make_error(ErrorCode::InvalidArgument, "manifest `schema_version` must be 1");
    }
    for (const char* field : {"adapter_id", "base_model_sha256", "adapter_sha256", "artifact_path",
                              "dataset_manifest_sha256", "eval_id", "created_at"}) {
        const json* value = str(field);
        if (value == nullptr) {
            return make_error(ErrorCode::InvalidArgument,
                              std::string("manifest `") + field + "` is missing or not a string");
        }
    }
    m.adapter_id = str("adapter_id")->get<std::string>();
    m.base_model_sha256 = str("base_model_sha256")->get<std::string>();
    m.adapter_sha256 = str("adapter_sha256")->get<std::string>();
    m.artifact_path = str("artifact_path")->get<std::string>();
    m.dataset_manifest_sha256 = str("dataset_manifest_sha256")->get<std::string>();
    m.eval_id = str("eval_id")->get<std::string>();
    m.created_at = str("created_at")->get<std::string>();

    if (const auto hyper = root.find("hyperparameters"); hyper != root.end() && hyper->is_object()) {
        for (const auto& [key, value] : hyper->items()) {
            if (!value.is_string()) {
                return make_error(ErrorCode::InvalidArgument, "hyperparameter values must be strings");
            }
            m.hyperparameters[key] = value.get<std::string>();
        }
    }
    const auto baseline = root.find("baseline");
    const auto candidate = root.find("candidate");
    const auto gate = root.find("gate");
    if (baseline == root.end() || !parse_scores(*baseline, m.baseline)) {
        return make_error(ErrorCode::InvalidArgument, "manifest `baseline` scores are missing or invalid");
    }
    if (candidate == root.end() || !parse_scores(*candidate, m.candidate)) {
        return make_error(ErrorCode::InvalidArgument, "manifest `candidate` scores are missing or invalid");
    }
    if (gate == root.end() || !parse_gate(*gate, m.gate)) {
        return make_error(ErrorCode::InvalidArgument, "manifest `gate` result is missing or invalid");
    }
    if (auto reason = check_manifest(m); !reason.empty()) {
        return make_error(ErrorCode::InvalidArgument, std::move(reason));
    }
    return m;
}

AdapterBook::AdapterBook(fs::path root) : root_(std::move(root)) {}

fs::path AdapterBook::manifest_path(const std::string& adapter_id) const {
    return root_ / "adapters" / adapter_id / "manifest.json";
}

fs::path AdapterBook::pointer_path() const {
    return root_ / "active.json";
}

Result<void> AdapterBook::install(const AdapterManifest& manifest, bool overwrite) {
    if (auto reason = check_manifest(manifest); !reason.empty()) {
        return make_error(ErrorCode::InvalidArgument, std::move(reason));
    }
    const fs::path path = manifest_path(manifest.adapter_id);
    std::error_code ec;
    if (!overwrite && fs::exists(path, ec)) {
        return make_error(ErrorCode::AlreadyExists,
                          "adapter '" + manifest.adapter_id + "' is already installed");
    }
    return atomic_write_file(path, to_json(manifest));
}

Result<std::vector<AdapterManifest>> AdapterBook::list() const {
    std::vector<AdapterManifest> out;
    const fs::path dir = root_ / "adapters";
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        return out;
    }
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_directory(ec)) {
            continue;
        }
        auto manifest = read_manifest_file(it->path() / "manifest.json");
        if (!manifest) {
            return manifest.error();
        }
        out.push_back(std::move(manifest).value());
    }
    return out;
}

Result<AdapterManifest> AdapterBook::get(const std::string& adapter_id) const {
    if (!detail::valid_adapter_id(adapter_id)) {
        return make_error(ErrorCode::InvalidArgument,
                          "adapter id is not a safe identifier: '" + adapter_id + "'");
    }
    const fs::path path = manifest_path(adapter_id);
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return make_error(ErrorCode::NotFound, "no installed adapter '" + adapter_id + "'");
    }
    return read_manifest_file(path);
}

Result<std::optional<AdapterManifest>> AdapterBook::active(const std::string& base_model_sha256) const {
    auto pointer = read_pointer(pointer_path());
    if (!pointer) {
        return pointer.error();
    }
    const auto it = pointer.value().find(detail::lower(base_model_sha256));
    if (it == pointer.value().end() || it->second.active.empty()) {
        return std::optional<AdapterManifest>{};
    }
    auto manifest = get(it->second.active);
    if (!manifest) {
        return manifest.error();
    }
    return std::optional<AdapterManifest>(std::move(manifest).value());
}

Result<void> AdapterBook::activate(const std::string& adapter_id) {
    auto manifest = get(adapter_id);
    if (!manifest) {
        return manifest.error();
    }
    if (manifest.value().gate.decision != GateDecision::Keep) {
        return make_error(ErrorCode::InvalidState,
                          "adapter '" + adapter_id + "' was not kept by the eval gate (" +
                              to_string(manifest.value().gate.decision) + "): refusing to activate");
    }
    auto pointer = read_pointer(pointer_path());
    if (!pointer) {
        return pointer.error();
    }
    TargetEntry& entry = pointer.value()[detail::lower(manifest.value().base_model_sha256)];
    if (entry.active != adapter_id) {
        entry.previous = entry.active;
        entry.active = adapter_id;
    }
    return write_pointer(pointer_path(), pointer.value());
}

Result<void> AdapterBook::rollback(const std::string& base_model_sha256) {
    auto pointer = read_pointer(pointer_path());
    if (!pointer) {
        return pointer.error();
    }
    const std::string key = detail::lower(base_model_sha256);
    auto it = pointer.value().find(key);
    if (it == pointer.value().end() || (it->second.active.empty() && it->second.previous.empty())) {
        return make_error(ErrorCode::NotFound, "no adapter has been activated for this base model");
    }
    const std::string old_active = it->second.active;
    it->second.active = it->second.previous;
    it->second.previous = old_active;
    return write_pointer(pointer_path(), pointer.value());
}

}  // namespace hcs::train
