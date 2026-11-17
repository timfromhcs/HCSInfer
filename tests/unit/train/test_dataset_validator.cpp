#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include <nlohmann/json.hpp>

#include "hcs/train/dataset_validator.hpp"

using namespace hcs::train;
using nlohmann::json;

namespace {

json good_record(const std::string& id = "r1", const std::string& answer = "Use std::sort.") {
    return json{{"id", id},
                {"licence", "Apache-2.0"},
                {"source", "https://example.org/dataset"},
                {"generator", "human"},
                {"messages", json::array({json{{"role", "user"}, {"content", "How do I sort a vector?"}},
                                          json{{"role", "assistant"}, {"content", answer}}})}};
}

std::string line(const json& j) {
    return j.dump() + "\n";
}

bool has_issue(const ValidationReport& r, IssueKind k, std::size_t line_no = 0) {
    return std::any_of(r.issues.begin(), r.issues.end(),
                       [&](const Issue& i) { return i.kind == k && (line_no == 0 || i.line == line_no); });
}

ValidationReport run(const std::string& text, DatasetLimits l = {}, DatasetPolicy p = {}) {
    return DatasetValidator(l, p).validate_string(text);
}

}  // namespace

TEST(DatasetValidator, AcceptsAWellFormedRecordAndCountsByLicence) {
    const auto rep = run(line(good_record("a")) + line(good_record("b", "Another answer.")));
    EXPECT_EQ(rep.accepted, 2u);
    EXPECT_EQ(rep.rejected, 0u);
    EXPECT_TRUE(rep.clean());
    EXPECT_EQ(rep.accepted_by_licence.at("Apache-2.0"), 2u);
    EXPECT_EQ(rep.accepted_lines, (std::vector<std::size_t>{1, 2}));
}

TEST(DatasetValidator, AcceptsToolCallTrajectories) {
    json rec = good_record();
    rec["messages"] = json::array(
        {json{{"role", "user"}, {"content", "List the files"}},
         json{{"role", "assistant"},
              {"content", nullptr},
              {"tool_calls", json::array({json{{"id", "c1"},
                                               {"function",
                                                {{"name", "run_command"},
                                                 {"arguments", "{\"argv\":[\"git\",\"status\"]}"}}}}})}},
         json{{"role", "tool"}, {"tool_call_id", "c1"}, {"content", "clean"}},
         json{{"role", "assistant"}, {"content", "The tree is clean."}}});
    rec["tools"] = json::array({json{{"type", "function"}}});
    const auto rep = run(line(rec));
    EXPECT_EQ(rep.accepted, 1u) << (rep.issues.empty() ? "" : rep.issues[0].detail);
}

TEST(DatasetValidator, RejectsToolCallsWithBadArguments) {
    json bad_json = good_record();
    bad_json["messages"][1] =
        json{{"role", "assistant"},
             {"content", ""},
             {"tool_calls", json::array({json{{"function", {{"name", "f"}, {"arguments", "{nope"}}}}})}};
    bad_json["messages"].push_back(json{{"role", "assistant"}, {"content", "done"}});
    EXPECT_TRUE(has_issue(run(line(bad_json)), IssueKind::SchemaViolation));

    json bad_name = bad_json;
    bad_name["messages"][1]["tool_calls"][0]["function"] = {{"name", "rm -rf /"},
                                                            {"arguments", json::object()}};
    EXPECT_TRUE(has_issue(run(line(bad_name)), IssueKind::SchemaViolation));

    json tool_from_user = good_record();
    tool_from_user["messages"][0]["tool_calls"] =
        json::array({json{{"function", {{"name", "f"}, {"arguments", json::object()}}}}});
    EXPECT_TRUE(has_issue(run(line(tool_from_user)), IssueKind::SchemaViolation));
}

TEST(DatasetValidator, LicenceIsRequiredAndMustBeAllowed) {
    json missing = good_record();
    missing.erase("licence");
    EXPECT_TRUE(has_issue(run(line(missing)), IssueKind::LicenceMissing, 1));

    json blank = good_record();
    blank["licence"] = "  ";
    EXPECT_TRUE(has_issue(run(line(blank)), IssueKind::LicenceMissing));

    for (const char* lic : {"CC-BY-NC-4.0", "CC-BY-SA-4.0", "GPL-3.0", "proprietary", "unknown", "other"}) {
        json r = good_record();
        r["licence"] = lic;
        const auto rep = run(line(r));
        EXPECT_EQ(rep.accepted, 0u) << lic;
        EXPECT_TRUE(has_issue(rep, IssueKind::LicenceNotAllowed)) << lic;
    }

    json mixed_case = good_record();
    mixed_case["licence"] = " apache-2.0 ";
    const auto rep = run(line(mixed_case));
    EXPECT_EQ(rep.accepted, 1u);
    EXPECT_EQ(rep.accepted_by_licence.at("Apache-2.0"), 1u);  // canonical spelling

    DatasetPolicy strict;
    strict.allowed_licences = {"MIT"};
    EXPECT_EQ(run(line(good_record()), {}, strict).accepted, 0u);
}

TEST(DatasetValidator, RejectsClaudeDistillationProvenance) {
    for (const char* gen : {"claude-3-opus", "Claude Sonnet 4.5", "anthropic/fable-opus", "OPUS", "haiku"}) {
        json r = good_record();
        r["generator"] = gen;
        const auto rep = run(line(r));
        EXPECT_EQ(rep.accepted, 0u) << gen;
        EXPECT_TRUE(has_issue(rep, IssueKind::ForbiddenProvenance)) << gen;
    }
    json by_source = good_record();
    by_source["source"] = "https://huggingface.co/datasets/someone/claude-code-traces";
    EXPECT_TRUE(has_issue(run(line(by_source)), IssueKind::ForbiddenProvenance));

    json no_generator = good_record();
    no_generator.erase("generator");
    EXPECT_TRUE(has_issue(run(line(no_generator)), IssueKind::SchemaViolation));

    json ok = good_record();
    ok["generator"] = "Qwen3-Coder-30B-A3B";
    EXPECT_EQ(run(line(ok)).accepted, 1u);
}

TEST(DatasetValidator, SchemaViolationsAreReportedWithLineNumbers) {
    std::string text;
    text += line(good_record("ok"));  // 1
    text += "{not json\n";            // 2
    text += "[1,2,3]\n";              // 3
    json no_assistant = good_record();
    no_assistant["messages"].erase(no_assistant["messages"].begin() + 1);
    text += line(no_assistant);  // 4
    json ends_with_user = good_record();
    ends_with_user["messages"].push_back(json{{"role", "user"}, {"content", "more"}});
    text += line(ends_with_user);  // 5
    json bad_role = good_record();
    bad_role["messages"][0]["role"] = "admin";
    text += line(bad_role);  // 6
    json empty_msgs = good_record();
    empty_msgs["messages"] = json::array();
    text += line(empty_msgs);  // 7
    json no_id = good_record();
    no_id.erase("id");
    text += line(no_id);  // 8

    const auto rep = run(text);
    EXPECT_EQ(rep.accepted, 1u);
    EXPECT_EQ(rep.rejected, 7u);
    EXPECT_TRUE(has_issue(rep, IssueKind::MalformedJson, 2));
    EXPECT_TRUE(has_issue(rep, IssueKind::SchemaViolation, 3));
    EXPECT_TRUE(has_issue(rep, IssueKind::SchemaViolation, 4));
    EXPECT_TRUE(has_issue(rep, IssueKind::SchemaViolation, 5));
    EXPECT_TRUE(has_issue(rep, IssueKind::SchemaViolation, 6));
    EXPECT_TRUE(has_issue(rep, IssueKind::SchemaViolation, 7));
    EXPECT_TRUE(has_issue(rep, IssueKind::SchemaViolation, 8));
    EXPECT_FALSE(rep.clean());
}

TEST(DatasetValidator, DeduplicatesAfterWhitespaceAndCaseNormalisation) {
    const auto rep =
        run(line(good_record("a", "Use std::sort.")) + line(good_record("b", "  use   STD::sort. \n")) +
            line(good_record("c", "Something else.")));
    EXPECT_EQ(rep.accepted, 2u);
    EXPECT_EQ(rep.duplicates, 1u);
    EXPECT_TRUE(has_issue(rep, IssueKind::Duplicate, 2));
}

TEST(DatasetValidator, EnforcesSizeCapsWithoutBufferingHostileLines) {
    DatasetLimits l;
    l.max_line_bytes = 512;
    std::string text = line(good_record("a"));
    text += std::string(100'000, 'x') + "\n";  // far over the line cap
    text += line(good_record("b", "Second answer."));
    const auto rep = run(text, l);
    EXPECT_EQ(rep.accepted, 2u);  // the oversize line is skipped, reading resumes at the next line
    EXPECT_TRUE(has_issue(rep, IssueKind::LineTooLong, 2));

    DatasetLimits m;
    m.max_message_bytes = 16;
    EXPECT_TRUE(has_issue(run(line(good_record("a", std::string(64, 'y'))), m), IssueKind::LimitExceeded));

    DatasetLimits n;
    n.max_messages = 1;
    EXPECT_TRUE(has_issue(run(line(good_record()), n), IssueKind::LimitExceeded));

    DatasetLimits recs;
    recs.max_records = 2;
    const auto capped = run(line(good_record("a", "one")) + line(good_record("b", "two")) +
                                line(good_record("c", "three")) + line(good_record("d", "four")),
                            recs);
    EXPECT_EQ(capped.accepted, 2u);
    EXPECT_TRUE(capped.truncated_by_limits);
    EXPECT_FALSE(capped.clean());

    DatasetLimits total;
    total.max_total_bytes = 200;
    const auto big =
        run(line(good_record("a", "one")) + line(good_record("b", "two")) + line(good_record("c", "three")),
            total);
    EXPECT_TRUE(big.truncated_by_limits);
}

TEST(DatasetValidator, RejectsDeeplyNestedJsonWithoutCrashing) {
    const std::string deep = std::string(20'000, '[') + std::string(20'000, ']') + "\n";
    const auto rep = run(deep);
    EXPECT_EQ(rep.accepted, 0u);
    EXPECT_TRUE(has_issue(rep, IssueKind::MalformedJson, 1));
}

TEST(DatasetValidator, RejectsRecordsContainingSecrets) {
    const std::string tok = std::string("hf_") + std::string(34, 'a');
    json r = good_record("a", "My token is " + tok);
    EXPECT_TRUE(has_issue(run(line(r)), IssueKind::SecretDetected));
    json key = good_record("b", "-----BEGIN RSA PRIVATE KEY-----\nMIIE");
    EXPECT_TRUE(has_issue(run(line(key)), IssueKind::SecretDetected));
    json aws = good_record("c", "key AKIAABCDEFGHIJKLMNOP here");
    EXPECT_TRUE(has_issue(run(line(aws)), IssueKind::SecretDetected));
    // Short look-alikes and mid-word matches are not flagged.
    json benign = good_record("d", "Use the hf_ prefix and the sk- flag; ASK-ME and AKIA are fine.");
    EXPECT_EQ(run(line(benign)).accepted, 1u);

    DatasetPolicy off;
    off.reject_secrets = false;
    EXPECT_EQ(run(line(r), {}, off).accepted, 1u);
}

TEST(DatasetValidator, IgnoresBlankLinesAndHandlesCrLf) {
    std::string text = "\n   \n" + good_record("a").dump() + "\r\n\r\n";
    const auto rep = run(text);
    EXPECT_EQ(rep.accepted, 1u);
    EXPECT_EQ(rep.rejected, 0u);
    EXPECT_EQ(rep.accepted_lines, (std::vector<std::size_t>{3}));
}

TEST(DatasetValidator, EmptyInputIsNotClean) {
    const auto rep = run("");
    EXPECT_EQ(rep.accepted, 0u);
    EXPECT_FALSE(rep.clean());
}

TEST(DatasetValidator, IssueListIsCappedButCountsAreExact) {
    DatasetLimits l;
    l.max_issues = 3;
    std::string text;
    for (int i = 0; i < 10; ++i) text += "{bad\n";
    const auto rep = run(text, l);
    EXPECT_EQ(rep.rejected, 10u);
    EXPECT_EQ(rep.issues.size(), 3u);
}
