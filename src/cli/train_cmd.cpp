#include "train_cmd.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "hcs/fs_util.hpp"
#include "hcs/result.hpp"
#include "hcs/train/adapter_book.hpp"
#include "hcs/train/dataset_validator.hpp"
#include "hcs/train/eval_gate.hpp"
#include "hcs/train/eval_report.hpp"
#include "hcs/train/pipeline.hpp"

namespace hcs::cli {

namespace fs = std::filesystem;
using nlohmann::json;
using namespace hcs::train;

namespace {

constexpr std::uint64_t kMaxDatasetBytes = 256ull << 20;  // bounded read for `train validate`
constexpr std::uint64_t kMaxInputBytes = 8ull << 20;      // bounded read for JSON inputs
constexpr std::size_t kMaxIssuesPrinted = 40;

const char* kTrainHelp =
    "Usage: HCSInfer train <subcommand>\n"
    "\n"
    "Local model fine-tuning support (ADR-012). This command does NOT run a trainer; it validates data,\n"
    "applies the evaluation gate and keeps/rolls back adapter versions. Adapter state lives under\n"
    "<state-dir>/train.\n"
    "\n"
    "  validate --dataset <file.jsonl> [--json]\n"
    "                        Validate a JSONL training set (licences, provenance, schema, duplicates)\n"
    "  gate --baseline <r.json> --candidate <r.json> [--min-gain X] [--max-drop Y] [--min-samples N] "
    "[--json]\n"
    "                        Apply the keep/rollback gate to two evaluation reports\n"
    "  adapter list [--json] List installed adapters (and which is active)\n"
    "  adapter show <id>     Print one adapter manifest\n"
    "  adapter activate <id> Promote an adapter (refused unless the gate kept it)\n"
    "  adapter rollback --base <sha256>\n"
    "                        Move the active pointer back to the previous adapter\n"
    "  pipeline --input <file.json> [--json]\n"
    "                        Gate a training result, record a manifest, promote on Keep\n";

const char* kValidateHelp =
    "Usage: HCSInfer train validate --dataset <file.jsonl> [--json]\n"
    "\n"
    "Reads the JSONL set with the DatasetValidator (allow-listed licences only, no forbidden provenance,\n"
    "schema checks, secret scan, duplicates); nothing is written.\n";

const char* kGateHelp =
    "Usage: HCSInfer train gate --baseline <report.json> --candidate <report.json>\n"
    "                           [--min-gain <0..1>] [--max-drop <0..1>] [--min-samples <n>] [--json]\n"
    "\n"
    "Keeps only if the held-out score improves by at least --min-gain AND no regression suite drops by more\n"
    "than --max-drop. Malformed or incomparable inputs are reported as decision 'invalid' (fail closed).\n";

const char* kAdapterHelp =
    "Usage: HCSInfer train adapter <list|show|activate|rollback> [args]\n"
    "\n"
    "  list [--json]                 Installed adapters and the active one per base model\n"
    "  show <id>                     One adapter manifest as JSON\n"
    "  activate <id>                 Promote it (only if the gate decision was 'keep')\n"
    "  rollback --base <sha256>      Move the pointer back (never deletes anything)\n";

const char* kPipelineHelp =
    "Usage: HCSInfer train pipeline --input <file.json> [--json]\n"
    "\n"
    "Input JSON: adapter_id, base_model_sha256, adapter_sha256, artifact_path, dataset_manifest_sha256,\n"
    "hyperparameters (optional object of strings), baseline, candidate (evaluation reports) and\n"
    "gate_policy (optional). Records a manifest for every decision; activates only on 'keep'.\n";

bool has_flag(std::vector<std::string>& v, const std::string& flag) {
    auto it = std::find(v.begin(), v.end(), flag);
    if (it == v.end())
        return false;
    v.erase(it);
    return true;
}

bool wants_help(const std::vector<std::string>& v) {
    return std::any_of(v.begin(), v.end(), [](const std::string& a) { return a == "--help" || a == "-h"; });
}

int fail(std::ostream& err, const Error& e) {
    err << "error: " << e.message << " [" << to_string(e.code) << "]\n";
    return e.exit_code();
}

int fail(std::ostream& err, ErrorCode code, const std::string& message) {
    return fail(err, Error(code, message));
}

bool take_option(std::vector<std::string>& v, const std::string& flag, std::string& value, bool& present) {
    present = false;
    auto it = std::find(v.begin(), v.end(), flag);
    if (it == v.end())
        return true;
    if (it + 1 == v.end())
        return false;
    value = *(it + 1);
    present = true;
    v.erase(it, it + 2);
    return true;
}

std::string now_iso8601_utc() {
    using std::chrono::system_clock;
    const std::time_t t = system_clock::to_time_t(system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// Parses a ratio in [0, 1] for a gate threshold.
Result<double> parse_ratio(const std::string& value) {
    try {
        std::size_t used = 0;
        const double d = std::stod(value, &used);
        if (used != value.size() || !std::isfinite(d) || d < 0.0 || d > 1.0) {
            return make_error(ErrorCode::InvalidArgument, "expected a number in [0, 1], got '" + value + "'");
        }
        return d;
    } catch (const std::exception&) {
        return make_error(ErrorCode::InvalidArgument, "expected a number in [0, 1], got '" + value + "'");
    }
}

Result<std::size_t> parse_positive_int(const std::string& value) {
    if (value.empty() || value.size() > 9 || value.find_first_not_of("0123456789") != std::string::npos) {
        return make_error(ErrorCode::InvalidArgument, "expected a positive integer, got '" + value + "'");
    }
    const auto n = static_cast<std::size_t>(std::stoull(value));
    if (n == 0) {
        return make_error(ErrorCode::InvalidArgument, "expected a positive integer, got '0'");
    }
    return n;
}

json validation_to_json(const std::string& path, const ValidationReport& rep) {
    json issues = json::array();
    for (const auto& issue : rep.issues) {
        if (issues.size() >= 100) {
            break;
        }
        issues.push_back(
            {{"line", issue.line}, {"kind", std::string(to_string(issue.kind))}, {"detail", issue.detail}});
    }
    json licences = json::object();
    for (const auto& [licence, count] : rep.accepted_by_licence) {
        licences[licence] = count;
    }
    return json{{"path", path},
                {"lines_read", rep.lines_read},
                {"accepted", rep.accepted},
                {"rejected", rep.rejected},
                {"duplicates", rep.duplicates},
                {"bytes_read", rep.bytes_read},
                {"truncated_by_limits", rep.truncated_by_limits},
                {"clean", rep.clean()},
                {"accepted_by_licence", std::move(licences)},
                {"issues", std::move(issues)}};
}

int cmd_validate(std::vector<std::string> args, std::ostream& out, std::ostream& err) {
    if (wants_help(args)) {
        out << kValidateHelp;
        return 0;
    }
    const bool as_json = has_flag(args, "--json");
    std::string dataset;
    bool present = false;
    if (!take_option(args, "--dataset", dataset, present) || !present)
        return fail(err, ErrorCode::InvalidArgument, "validate needs --dataset <file.jsonl>");
    if (!args.empty())
        return fail(err, ErrorCode::InvalidArgument, "unexpected argument: " + args.front());

    auto text = read_file_limited(fs::path(dataset), kMaxDatasetBytes);
    if (!text)
        return fail(err, text.error());
    DatasetValidator validator;
    const ValidationReport rep = validator.validate_string(text.value());

    if (as_json) {
        out << validation_to_json(dataset, rep).dump(2) << "\n";
        return 0;
    }
    out << "dataset:            " << dataset << "\n"
        << "lines read:         " << rep.lines_read << "\n"
        << "accepted:           " << rep.accepted << "\n"
        << "rejected:           " << rep.rejected << " (duplicates: " << rep.duplicates << ")\n"
        << "bytes read:         " << rep.bytes_read << "\n"
        << "truncated by limits:" << (rep.truncated_by_limits ? " yes\n" : " no\n")
        << "clean:              " << (rep.clean() ? "yes" : "no") << "\n";
    if (!rep.accepted_by_licence.empty()) {
        out << "accepted licences:  ";
        bool first = true;
        for (const auto& [licence, count] : rep.accepted_by_licence) {
            out << (first ? "" : ", ") << licence << "=" << count;
            first = false;
        }
        out << "\n";
    }
    if (!rep.issues.empty()) {
        out << "issues:\n";
        std::size_t shown = 0;
        for (const auto& issue : rep.issues) {
            if (shown++ >= kMaxIssuesPrinted) {
                out << "  ... " << (rep.issues.size() - kMaxIssuesPrinted) << " more\n";
                break;
            }
            out << "  line " << issue.line << " [" << to_string(issue.kind) << "] " << issue.detail << "\n";
        }
    }
    return 0;
}

int cmd_gate(std::vector<std::string> args, std::ostream& out, std::ostream& err) {
    if (wants_help(args)) {
        out << kGateHelp;
        return 0;
    }
    const bool as_json = has_flag(args, "--json");
    std::string baseline_path;
    std::string candidate_path;
    bool present = false;
    if (!take_option(args, "--baseline", baseline_path, present) || !present)
        return fail(err, ErrorCode::InvalidArgument, "gate needs --baseline <report.json>");
    if (!take_option(args, "--candidate", candidate_path, present) || !present)
        return fail(err, ErrorCode::InvalidArgument, "gate needs --candidate <report.json>");

    GatePolicy policy;
    std::string value;
    if (!take_option(args, "--min-gain", value, present))
        return fail(err, ErrorCode::InvalidArgument, "--min-gain needs a value");
    if (present) {
        auto r = parse_ratio(value);
        if (!r)
            return fail(err, r.error());
        policy.min_heldout_gain = r.value();
    }
    if (!take_option(args, "--max-drop", value, present))
        return fail(err, ErrorCode::InvalidArgument, "--max-drop needs a value");
    if (present) {
        auto r = parse_ratio(value);
        if (!r)
            return fail(err, r.error());
        policy.max_suite_drop = r.value();
    }
    if (!take_option(args, "--min-samples", value, present))
        return fail(err, ErrorCode::InvalidArgument, "--min-samples needs a value");
    if (present) {
        auto r = parse_positive_int(value);
        if (!r)
            return fail(err, r.error());
        policy.min_heldout_samples = r.value();
    }
    if (!args.empty())
        return fail(err, ErrorCode::InvalidArgument, "unexpected argument: " + args.front());

    auto baseline = parse_eval_report_file(fs::path(baseline_path), kMaxInputBytes);
    if (!baseline)
        return fail(err, baseline.error());
    auto candidate = parse_eval_report_file(fs::path(candidate_path), kMaxInputBytes);
    if (!candidate)
        return fail(err, candidate.error());

    const std::string incomparable = [&] {
        std::string reason;
        return comparable(baseline.value(), candidate.value(), reason) ? std::string() : reason;
    }();
    const GateResult result =
        incomparable.empty() ? evaluate_gate(baseline.value().scores(), candidate.value().scores(), policy)
                             : GateResult{GateDecision::Invalid, incomparable, 0.0, {}};

    if (as_json) {
        json j{{"decision", to_string(result.decision)},
               {"reason", result.reason},
               {"heldout_delta", result.heldout_delta},
               {"regressed_suites", result.regressed_suites},
               {"baseline",
                {{"eval_id", baseline.value().eval_id},
                 {"heldout", baseline.value().heldout_score},
                 {"heldout_samples", baseline.value().heldout_samples}}},
               {"candidate",
                {{"eval_id", candidate.value().eval_id},
                 {"heldout", candidate.value().heldout_score},
                 {"heldout_samples", candidate.value().heldout_samples}}}};
        out << j.dump(2) << "\n";
        return 0;
    }
    out << "decision:       " << to_string(result.decision) << "\n"
        << "reason:         " << result.reason << "\n"
        << "held-out delta: " << result.heldout_delta << "\n";
    if (!result.regressed_suites.empty()) {
        out << "regressed:      ";
        for (std::size_t i = 0; i < result.regressed_suites.size(); ++i)
            out << (i ? ", " : "") << result.regressed_suites[i];
        out << "\n";
    }
    return 0;
}

int cmd_adapter(std::vector<std::string> args, AdapterBook& book, std::ostream& out, std::ostream& err) {
    if (args.empty() || args.front() == "--help" || args.front() == "-h") {
        out << kAdapterHelp;
        return args.empty() ? exit_code(ErrorCode::InvalidArgument) : 0;
    }
    const std::string sub = args.front();
    args.erase(args.begin());

    if (sub == "list") {
        const bool as_json = has_flag(args, "--json");
        if (!args.empty())
            return fail(err, ErrorCode::InvalidArgument, "unexpected argument: " + args.front());
        auto adapters = book.list();
        if (!adapters)
            return fail(err, adapters.error());
        json arr = json::array();
        for (const auto& m : adapters.value()) {
            auto active = book.active(m.base_model_sha256);
            bool is_active = false;
            if (active) {
                // Bind the optional to a reference inside its own guard so the access is provably checked.
                const auto& current = active.value();
                if (current.has_value() && current->adapter_id == m.adapter_id) {
                    is_active = true;
                }
            }
            arr.push_back({{"adapter_id", m.adapter_id},
                           {"base_model_sha256", m.base_model_sha256},
                           {"eval_id", m.eval_id},
                           {"decision", to_string(m.gate.decision)},
                           {"created_at", m.created_at},
                           {"active", is_active}});
        }
        if (as_json) {
            out << arr.dump(2) << "\n";
            return 0;
        }
        if (arr.empty()) {
            out << "No adapters installed. Use `train pipeline --input <file.json>`.\n";
            return 0;
        }
        out << std::left << std::setw(28) << "ADAPTER" << std::setw(12) << "DECISION" << std::setw(22)
            << "EVAL" << "ACTIVE\n";
        for (const auto& entry : arr) {
            out << std::left << std::setw(28) << entry["adapter_id"].get<std::string>() << std::setw(12)
                << entry["decision"].get<std::string>() << std::setw(22)
                << entry["eval_id"].get<std::string>() << (entry["active"].get<bool>() ? "yes" : "no")
                << "\n";
        }
        return 0;
    }
    if (sub == "show") {
        if (args.size() != 1)
            return fail(err, ErrorCode::InvalidArgument, "show needs exactly one <id>");
        auto manifest = book.get(args[0]);
        if (!manifest)
            return fail(err, manifest.error());
        out << to_json(manifest.value()) << "\n";
        return 0;
    }
    if (sub == "activate") {
        if (args.size() != 1)
            return fail(err, ErrorCode::InvalidArgument, "activate needs exactly one <id>");
        auto r = book.activate(args[0]);
        if (!r)
            return fail(err, r.error());
        out << "activated " << args[0] << "\n";
        return 0;
    }
    if (sub == "rollback") {
        std::string base;
        bool present = false;
        if (!take_option(args, "--base", base, present) || !present)
            return fail(err, ErrorCode::InvalidArgument, "rollback needs --base <sha256>");
        if (!args.empty())
            return fail(err, ErrorCode::InvalidArgument, "unexpected argument: " + args.front());
        auto r = book.rollback(base);
        if (!r)
            return fail(err, r.error());
        out << "rolled back the active adapter for " << base << "\n";
        return 0;
    }
    return fail(err, ErrorCode::InvalidArgument, "unknown adapter subcommand: " + sub);
}

int cmd_pipeline(std::vector<std::string> args, AdapterBook& book, std::ostream& out, std::ostream& err) {
    if (wants_help(args)) {
        out << kPipelineHelp;
        return 0;
    }
    const bool as_json = has_flag(args, "--json");
    std::string input;
    bool present = false;
    if (!take_option(args, "--input", input, present) || !present)
        return fail(err, ErrorCode::InvalidArgument, "pipeline needs --input <file.json>");
    if (!args.empty())
        return fail(err, ErrorCode::InvalidArgument, "unexpected argument: " + args.front());

    auto text = read_file_limited(fs::path(input), kMaxInputBytes);
    if (!text)
        return fail(err, text.error());
    const json root = json::parse(text.value(), nullptr, false);
    if (root.is_discarded() || !root.is_object())
        return fail(err, ErrorCode::InvalidArgument, "pipeline input is not a JSON object");

    PipelineRequest req;
    const auto str_or = [&](const char* field) -> Result<std::string> {
        const auto it = root.find(field);
        if (it == root.end() || !it->is_string())
            return make_error(ErrorCode::InvalidArgument,
                              std::string("input `") + field + "` must be a string");
        return it->get<std::string>();
    };
    for (const char* field :
         {"adapter_id", "base_model_sha256", "adapter_sha256", "artifact_path", "dataset_manifest_sha256"}) {
        auto r = str_or(field);
        if (!r)
            return fail(err, r.error());
        const std::string v = r.value();
        if (std::string(field) == "adapter_id")
            req.adapter_id = v;
        else if (std::string(field) == "base_model_sha256")
            req.base_model_sha256 = v;
        else if (std::string(field) == "adapter_sha256")
            req.adapter_sha256 = v;
        else if (std::string(field) == "artifact_path")
            req.artifact_path = v;
        else
            req.dataset_manifest_sha256 = v;
    }
    if (const auto hyper = root.find("hyperparameters"); hyper != root.end()) {
        if (!hyper->is_object())
            return fail(err, ErrorCode::InvalidArgument, "input `hyperparameters` must be an object");
        for (const auto& [key, value] : hyper->items()) {
            if (!value.is_string())
                return fail(err, ErrorCode::InvalidArgument, "hyperparameter values must be strings");
            req.hyperparameters[key] = value.get<std::string>();
        }
    }
    const auto baseline = root.find("baseline");
    const auto candidate = root.find("candidate");
    if (baseline == root.end() || candidate == root.end())
        return fail(err, ErrorCode::InvalidArgument, "input needs `baseline` and `candidate` reports");
    auto parsed_baseline = parse_eval_report(baseline->dump());
    if (!parsed_baseline)
        return fail(err, parsed_baseline.error());
    auto parsed_candidate = parse_eval_report(candidate->dump());
    if (!parsed_candidate)
        return fail(err, parsed_candidate.error());
    req.baseline = std::move(parsed_baseline).value();
    req.candidate = std::move(parsed_candidate).value();
    if (const auto policy = root.find("gate_policy"); policy != root.end() && policy->is_object()) {
        const auto set_ratio = [&](const char* field, double& target) -> std::string {
            const auto it = policy->find(field);
            if (it == policy->end())
                return {};
            if (!it->is_number() || !std::isfinite(it->get<double>()) || it->get<double>() < 0.0 ||
                it->get<double>() > 1.0)
                return std::string("gate_policy `") + field + "` must be a number in [0, 1]";
            target = it->get<double>();
            return {};
        };
        if (auto e = set_ratio("min_heldout_gain", req.policy.min_heldout_gain); !e.empty())
            return fail(err, ErrorCode::InvalidArgument, e);
        if (auto e = set_ratio("max_suite_drop", req.policy.max_suite_drop); !e.empty())
            return fail(err, ErrorCode::InvalidArgument, e);
        if (const auto it = policy->find("min_heldout_samples"); it != policy->end()) {
            if (!it->is_number_unsigned() || it->get<std::size_t>() == 0)
                return fail(err, ErrorCode::InvalidArgument,
                            "gate_policy `min_heldout_samples` must be a positive integer");
            req.policy.min_heldout_samples = it->get<std::size_t>();
        }
    }
    req.created_at = now_iso8601_utc();

    auto outcome = run_train_pipeline(req, book);
    if (!outcome)
        return fail(err, outcome.error());

    const PipelineOutcome& o = outcome.value();
    if (as_json) {
        json j{{"adapter_id", o.adapter_id},
               {"decision", to_string(o.gate.decision)},
               {"reason", o.gate.reason},
               {"heldout_delta", o.gate.heldout_delta},
               {"regressed_suites", o.gate.regressed_suites},
               {"installed", o.installed},
               {"activated", o.activated},
               {"manifest_path", o.manifest_path}};
        out << j.dump(2) << "\n";
        return 0;
    }
    out << "adapter:    " << o.adapter_id << "\n"
        << "decision:   " << to_string(o.gate.decision) << "\n"
        << "reason:     " << o.gate.reason << "\n"
        << "installed:  " << (o.installed ? "yes" : "no") << "\n"
        << "activated:  " << (o.activated ? "yes" : "no") << "\n"
        << "manifest:   " << o.manifest_path << "\n";
    return 0;
}

}  // namespace

const char* train_help() {
    return kTrainHelp;
}

int run_train(std::vector<std::string> args, const fs::path& state_dir, std::ostream& out,
              std::ostream& err) {
    if (args.empty()) {
        err << kTrainHelp;
        return exit_code(ErrorCode::InvalidArgument);
    }
    const std::string sub = args.front();
    if (sub == "--help" || sub == "-h" || sub == "help") {
        out << kTrainHelp;
        return 0;
    }
    args.erase(args.begin());

    AdapterBook book(state_dir / "train");
    if (sub == "validate")
        return cmd_validate(std::move(args), out, err);
    if (sub == "gate")
        return cmd_gate(std::move(args), out, err);
    if (sub == "adapter")
        return cmd_adapter(std::move(args), book, out, err);
    if (sub == "pipeline")
        return cmd_pipeline(std::move(args), book, out, err);

    err << "error: unknown train subcommand '" << sub << "'\n\n" << kTrainHelp;
    return exit_code(ErrorCode::InvalidArgument);
}

}  // namespace hcs::cli
