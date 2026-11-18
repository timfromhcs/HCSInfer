#include "hcs/train/rsi_loop.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <optional>
#include <utility>

#include "hcs/train/pipeline.hpp"
#include "train_util.hpp"

namespace hcs::train {

using nlohmann::json;

namespace {

constexpr std::size_t kMaxFailureEntries = 100000;
constexpr std::size_t kMaxFailureText = 128;

std::string clean_text(const json& v) {
    if (!v.is_string()) {
        return {};
    }
    std::string s = v.get<std::string>();
    if (s.size() > kMaxFailureText) {
        std::size_t keep = kMaxFailureText;
        while (keep > 0 && (static_cast<unsigned char>(s[keep]) & 0xC0U) == 0x80U) {
            --keep;
        }
        s.resize(keep);
    }
    for (char& c : s) {
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) == 0x7f) {
            c = ' ';
        }
    }
    return s;
}

std::size_t saturating_mul(std::size_t a, std::size_t b) {
    if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a) {
        return std::numeric_limits<std::size_t>::max();
    }
    return a * b;
}

// A hook is user-supplied code: an exception must end the loop cleanly, never escape or half-apply a cycle.
template <typename Fn>
auto guarded(const char* what, Fn&& fn) -> decltype(fn()) {
    try {
        return fn();
    } catch (const std::exception& e) {
        return make_error(ErrorCode::Internal, std::string(what) + " threw: " + e.what());
    } catch (...) {
        return make_error(ErrorCode::Internal, std::string(what) + " threw an unknown exception");
    }
}

}  // namespace

Result<std::vector<EvalFailure>> parse_eval_failures(std::string_view text) {
    if (text.empty()) {
        return make_error(ErrorCode::InvalidArgument, "eval results are empty");
    }
    if (detail::json_depth(text) > detail::kMaxJsonDepth) {
        return make_error(ErrorCode::InvalidArgument, "eval results nest too deeply");
    }
    const json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return make_error(ErrorCode::InvalidArgument, "eval results are not a JSON object");
    }
    std::vector<EvalFailure> out;
    const auto results = root.find("results");
    if (results == root.end()) {
        return out;
    }
    if (!results->is_array()) {
        return make_error(ErrorCode::InvalidArgument, "`results` must be an array");
    }
    if (results->size() > kMaxFailureEntries) {
        return make_error(ErrorCode::InvalidArgument, "`results` has too many entries");
    }
    for (const auto& entry : *results) {
        if (!entry.is_object()) {
            return make_error(ErrorCode::InvalidArgument, "a `results` entry is not an object");
        }
        const auto pass = entry.find("pass");
        if (pass == entry.end() || !pass->is_boolean()) {
            return make_error(ErrorCode::InvalidArgument, "a `results` entry has no boolean `pass`");
        }
        if (pass->get<bool>()) {
            continue;
        }
        EvalFailure f;
        if (const auto it = entry.find("id"); it != entry.end()) {
            f.id = clean_text(*it);
        }
        if (const auto it = entry.find("category"); it != entry.end()) {
            f.category = clean_text(*it);
        }
        if (const auto it = entry.find("reason"); it != entry.end()) {
            f.reason = clean_text(*it);
        }
        out.push_back(std::move(f));
    }
    return out;
}

std::vector<Proposal> reflect(const std::vector<EvalFailure>& failures, const RsiPolicy& policy) {
    std::map<std::string, std::size_t> failed;
    for (const auto& f : failures) {
        ++failed[f.category.empty() ? "uncategorised" : f.category];
    }
    std::vector<std::pair<std::string, std::size_t>> order(failed.begin(), failed.end());
    std::stable_sort(order.begin(), order.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
    std::vector<Proposal> out;
    for (const auto& [category, n] : order) {
        Proposal p;
        p.kind = "add_training_data";
        p.category = category;
        p.count = std::min(saturating_mul(n, policy.examples_per_failure), policy.max_examples_per_category);
        p.rationale =
            std::to_string(n) + " held-out task(s) of category '" + category +
            "' fail; add new, validated, eval-disjoint training examples of that kind (never the eval tasks)";
        out.push_back(std::move(p));
    }
    if (out.empty()) {
        Proposal p;
        p.kind = "stop";
        p.rationale = "no failing held-out task: nothing to propose";
        out.push_back(std::move(p));
    }
    return out;
}

json to_json(const Proposal& p) {
    return {{"kind", p.kind}, {"category", p.category}, {"count", p.count}, {"rationale", p.rationale}};
}

json to_json(const CycleRecord& c) {
    json props = json::array();
    for (const auto& p : c.proposals) {
        props.push_back(to_json(p));
    }
    return {{"cycle", c.cycle},
            {"proposals", props},
            {"adapter_id", c.adapter_id},
            {"decision", to_string(c.decision)},
            {"reason", c.reason},
            {"baseline_heldout", c.baseline_heldout},
            {"candidate_heldout", c.candidate_heldout},
            {"installed", c.installed},
            {"activated", c.activated}};
}

Result<RsiOutcome> run_rsi(const RsiPolicy& policy_in, const std::string& base_model_sha256,
                           AdapterBook& book, const RsiHooks& hooks, const std::atomic<bool>* cancel) {
    if (!hooks.evaluate || !hooks.train_candidate) {
        return make_error(ErrorCode::InvalidArgument, "RSI hooks are not set");
    }
    if (!detail::is_hex64(base_model_sha256)) {
        return make_error(ErrorCode::InvalidArgument, "base_model_sha256 must be 64 hex characters");
    }
    RsiPolicy policy = policy_in;
    policy.max_cycles = std::clamp(policy.max_cycles, 1, 10);
    policy.patience = std::max(policy.patience, 1);
    const auto t0 = std::chrono::steady_clock::now();
    const auto cancelled = [&] { return cancel != nullptr && cancel->load(); };
    const auto out_of_budget = [&] { return std::chrono::steady_clock::now() - t0 > policy.budget; };

    auto active = book.active(base_model_sha256);
    if (!active) {
        return active.error();
    }
    const std::optional<AdapterManifest>& active_manifest = active.value();
    std::string current = active_manifest.has_value() ? active_manifest->adapter_id : std::string();

    RsiOutcome out;
    auto baseline = guarded("evaluate(baseline)", [&] { return hooks.evaluate(current); });
    if (!baseline) {
        out.stop_reason = "baseline evaluation failed: " + baseline.error().message;
        out.final_adapter = current;
        return out;
    }
    Evaluation base = std::move(baseline).value();

    int stale = 0;
    for (int cycle = 1; cycle <= policy.max_cycles; ++cycle) {
        if (cancelled()) {
            out.stop_reason = "cancelled";
            break;
        }
        if (out_of_budget()) {
            out.stop_reason = "wall-clock budget exhausted";
            break;
        }
        CycleRecord rec;
        rec.cycle = cycle;
        rec.proposals = reflect(base.failures, policy);
        rec.baseline_heldout = base.report.heldout_score;
        if (rec.proposals.size() == 1 && rec.proposals[0].kind == "stop") {
            rec.reason = rec.proposals[0].rationale;
            out.stop_reason = rec.reason;
            out.cycles.push_back(std::move(rec));
            break;
        }

        auto trained =
            guarded("train_candidate", [&] { return hooks.train_candidate(rec.proposals, cycle); });
        if (!trained) {
            rec.reason = "training failed: " + trained.error().message;
            out.stop_reason = rec.reason;
            out.cycles.push_back(std::move(rec));
            break;
        }
        const Candidate cand = std::move(trained).value();
        rec.adapter_id = cand.adapter_id;
        if (cancelled()) {
            rec.reason = "cancelled after training; the candidate was not evaluated or installed";
            out.stop_reason = "cancelled";
            out.cycles.push_back(std::move(rec));
            break;
        }
        if (out_of_budget()) {
            rec.reason = "budget exhausted after training; the candidate was not evaluated or installed";
            out.stop_reason = "wall-clock budget exhausted";
            out.cycles.push_back(std::move(rec));
            break;
        }

        auto evaluated = guarded("evaluate(candidate)", [&] { return hooks.evaluate(cand.adapter_id); });
        if (!evaluated) {
            rec.reason = "evaluation failed: " + evaluated.error().message;
            out.stop_reason = rec.reason;
            out.cycles.push_back(std::move(rec));
            break;
        }
        const Evaluation candidate = std::move(evaluated).value();
        rec.candidate_heldout = candidate.report.heldout_score;
        if (cancelled()) {
            rec.reason = "cancelled after evaluation; the candidate was not installed";
            out.stop_reason = "cancelled";
            out.cycles.push_back(std::move(rec));
            break;
        }

        std::string why_not_comparable;
        if (!comparable(base.report, candidate.report, why_not_comparable)) {
            // Different suite or task set: the numbers mean nothing against each other. Same as a Rollback,
            // but nothing is installed because the gate cannot be evaluated.
            rec.decision = GateDecision::Invalid;
            rec.reason = "not comparable: " + why_not_comparable;
            out.cycles.push_back(std::move(rec));
            ++stale;
        } else {
            PipelineRequest request;
            request.adapter_id = cand.adapter_id;
            request.base_model_sha256 = base_model_sha256;
            request.adapter_sha256 = cand.adapter_sha256;
            request.artifact_path = cand.artifact_path;
            request.dataset_manifest_sha256 = cand.dataset_manifest_sha256;
            request.hyperparameters = cand.hyperparameters;
            request.baseline = base.report;
            request.candidate = candidate.report;
            request.policy = policy.gate;
            request.created_at = cand.created_at;
            auto piped = run_train_pipeline(request, book);  // installs the manifest; activates only on Keep
            if (!piped) {
                rec.reason = "pipeline refused the candidate: " + piped.error().message;
                out.stop_reason = rec.reason;
                out.cycles.push_back(std::move(rec));
                break;
            }
            rec.decision = piped.value().gate.decision;
            rec.reason = piped.value().gate.reason;
            rec.installed = piped.value().installed;
            rec.activated = piped.value().activated;
            const bool kept = rec.activated;
            out.cycles.push_back(std::move(rec));
            if (kept) {
                current = cand.adapter_id;
                base = candidate;  // the next cycle improves on what is now active
                stale = 0;
            } else {
                ++stale;  // rolled back: the previous adapter / base model stays active
            }
        }
        if (stale >= policy.patience) {
            out.stop_reason = "no accepted improvement in " + std::to_string(stale) + " consecutive cycle(s)";
            break;
        }
    }
    if (out.stop_reason.empty()) {
        out.stop_reason = "max cycles reached";
    }
    out.final_adapter = current;
    return out;
}

}  // namespace hcs::train
