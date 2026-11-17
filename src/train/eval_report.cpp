#include "hcs/train/eval_report.hpp"

#include <cmath>
#include <utility>

#include <nlohmann/json.hpp>

#include "hcs/fs_util.hpp"
#include "train_util.hpp"

namespace hcs::train {

using nlohmann::json;

namespace {

Result<EvalReport> bad(std::string message) {
    return make_error(ErrorCode::InvalidArgument, std::move(message));
}

bool score_ok(double v) {
    return std::isfinite(v) && v >= 0.0 && v <= 1.0;
}

// Reads a positive integer field; 0 or a non-integer is invalid.
bool read_count(const json& obj, const char* field, std::size_t& out) {
    const auto it = obj.find(field);
    if (it == obj.end()) {
        return false;
    }
    if (it->is_number_unsigned()) {
        const unsigned long long v = it->get<unsigned long long>();
        if (v == 0) {
            return false;
        }
        out = static_cast<std::size_t>(v);
        return true;
    }
    if (it->is_number_integer()) {
        const long long v = it->get<long long>();
        if (v <= 0) {
            return false;
        }
        out = static_cast<std::size_t>(v);
        return true;
    }
    return false;
}

bool read_score(const json& obj, const char* field, double& out) {
    const auto it = obj.find(field);
    if (it == obj.end() || !it->is_number()) {
        return false;
    }
    const double v = it->get<double>();
    if (!score_ok(v)) {
        return false;
    }
    out = v;
    return true;
}

}  // namespace

EvalScores EvalReport::scores() const {
    EvalScores s;
    s.heldout = heldout_score;
    s.heldout_samples = heldout_samples;
    for (const auto& [name, result] : regression) {
        s.regression[name] = result.score;
    }
    return s;
}

Result<EvalReport> parse_eval_report(std::string_view text) {
    if (text.empty()) {
        return bad("report is empty");
    }
    if (detail::json_depth(text) > detail::kMaxJsonDepth) {
        return bad("report JSON nests too deeply");
    }
    const json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return bad("report is not a JSON object");
    }

    const auto version_it = root.find("schema_version");
    if (version_it == root.end() || !version_it->is_number_integer() || version_it->get<int>() != 1) {
        return bad("`schema_version` must be the integer 1");
    }

    EvalReport out;
    out.schema_version = 1;

    const auto eval_it = root.find("eval_id");
    if (eval_it == root.end() || !eval_it->is_string() ||
        !detail::valid_eval_id(eval_it->get_ref<const std::string&>())) {
        return bad("`eval_id` must be a non-empty identifier (max 128, [A-Za-z0-9._-])");
    }
    out.eval_id = eval_it->get<std::string>();

    const auto set_it = root.find("set_sha256");
    if (set_it == root.end() || !set_it->is_string() ||
        !detail::is_hex64(set_it->get_ref<const std::string&>())) {
        return bad("`set_sha256` must be a 64-character hex hash of the frozen evaluation set");
    }
    out.set_sha256 = detail::lower(set_it->get<std::string>());

    const auto held_it = root.find("heldout");
    if (held_it == root.end() || !held_it->is_object()) {
        return bad("`heldout` must be an object with `samples` and `score`");
    }
    if (!read_count(*held_it, "samples", out.heldout_samples)) {
        return bad("`heldout.samples` must be a positive integer");
    }
    if (!read_score(*held_it, "score", out.heldout_score)) {
        return bad("`heldout.score` must be a finite number in [0, 1]");
    }

    const auto reg_it = root.find("regression");
    if (reg_it == root.end() || !reg_it->is_array() || reg_it->empty()) {
        return bad(
            "`regression` must be a non-empty array of suites (a regression-free claim cannot be proven)");
    }
    for (const auto& suite : *reg_it) {
        if (!suite.is_object()) {
            return bad("a regression suite is not an object");
        }
        const auto name_it = suite.find("name");
        if (name_it == suite.end() || !name_it->is_string() ||
            !detail::valid_identifier(name_it->get_ref<const std::string&>(), 64)) {
            return bad("regression suite `name` must be a plain identifier (max 64, [A-Za-z0-9._-])");
        }
        const std::string name = name_it->get<std::string>();
        RegressionSuiteResult entry;
        if (!read_count(suite, "samples", entry.samples)) {
            return bad("regression suite '" + name + "' needs a positive integer `samples`");
        }
        if (!read_score(suite, "score", entry.score)) {
            return bad("regression suite '" + name + "' `score` must be a finite number in [0, 1]");
        }
        if (!out.regression.emplace(name, entry).second) {
            return bad("regression suite '" + name + "' is listed twice");
        }
    }

    return out;
}

Result<EvalReport> parse_eval_report_file(const std::filesystem::path& path, std::uint64_t max_bytes) {
    auto text = read_file_limited(path, max_bytes);
    if (!text) {
        return text.error();
    }
    return parse_eval_report(text.value());
}

std::string to_json(const EvalReport& report) {
    json j;
    j["schema_version"] = report.schema_version;
    j["eval_id"] = report.eval_id;
    j["set_sha256"] = report.set_sha256;
    j["heldout"] = {{"samples", report.heldout_samples}, {"score", report.heldout_score}};
    json suites = json::array();
    for (const auto& [name, result] : report.regression) {
        suites.push_back({{"name", name}, {"samples", result.samples}, {"score", result.score}});
    }
    j["regression"] = std::move(suites);
    return j.dump(2);
}

bool comparable(const EvalReport& baseline, const EvalReport& candidate, std::string& reason) {
    if (baseline.eval_id != candidate.eval_id) {
        reason = "baseline is '" + baseline.eval_id + "' but candidate is '" + candidate.eval_id + "'";
        return false;
    }
    if (detail::lower(baseline.set_sha256) != detail::lower(candidate.set_sha256)) {
        reason = "baseline and candidate were evaluated against different task sets (set_sha256 differs)";
        return false;
    }
    return true;
}

}  // namespace hcs::train
