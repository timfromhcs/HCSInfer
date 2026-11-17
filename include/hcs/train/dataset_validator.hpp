#pragma once
// JSONL trajectory / SFT dataset validator for HCS Train (ADR-012).
//
// One JSON object per line:
//   {"id": "...", "licence": "<SPDX id>", "source": "<url or name>", "generator": "human" | "<model id>",
//    "messages": [{"role": "system|user|assistant|tool", "content": "...",
//                  "tool_calls": [{"function": {"name": "...", "arguments": {...} | "<json string>"}}],
//                  "tool_call_id": "..."}],
//    "tools": [ ... ]   // optional, must be an array of objects
//   }
//
// Policy enforced here (fail closed, every violation is reported with its 1-based line number):
//  - `licence` is required and must be on the allow-list (permissive by default); unknown, missing and
//    non-commercial / share-alike licences are rejected;
//  - `generator` is required; records whose assistant turns were produced by a forbidden model family
//    (default: Claude / Anthropic names) are rejected - HCS never trains on distilled Claude output;
//  - size caps (line, record, message count, per-message, total bytes) are enforced while reading, a hostile
//    multi-gigabyte line is never buffered;
//  - exact duplicates (after whitespace/case normalisation of the conversation) are dropped;
//  - obvious secrets (private keys, API-token shapes) are rejected.
#include <cstddef>
#include <cstdint>
#include <istream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hcs/result.hpp"

namespace hcs::train {

struct DatasetLimits {
    std::size_t max_line_bytes = 1024 * 1024;
    std::size_t max_records = 200'000;
    std::size_t max_total_bytes = 512ULL * 1024 * 1024;
    std::size_t max_messages = 64;
    std::size_t max_message_bytes = 256 * 1024;
    std::size_t max_issues = 1000;  // the report stops recording details (counts stay exact)
};

struct DatasetPolicy {
    // Canonical SPDX-style identifiers, compared case-insensitively.
    std::vector<std::string> allowed_licences = {"MIT", "Apache-2.0", "BSD-2-Clause", "BSD-3-Clause",
                                                 "ISC", "CC0-1.0",    "CC-BY-4.0",    "Unlicense"};
    // Lower-case substrings; matched against `generator` and `source`.
    std::vector<std::string> forbidden_generator_substrings = {"claude", "anthropic", "opus",
                                                               "sonnet", "haiku",     "fable"};
    bool require_tool_call_json = true;  // tool_calls[].function.arguments must parse as a JSON object
    bool reject_secrets = true;
};

enum class IssueKind {
    MalformedJson,
    LineTooLong,
    SchemaViolation,
    LicenceMissing,
    LicenceNotAllowed,
    ForbiddenProvenance,
    LimitExceeded,
    SecretDetected,
    Duplicate,
};

std::string_view to_string(IssueKind k) noexcept;

struct Issue {
    std::size_t line = 0;  // 1-based
    IssueKind kind = IssueKind::SchemaViolation;
    std::string detail;
};

struct ValidationReport {
    std::size_t lines_read = 0;
    std::size_t accepted = 0;
    std::size_t rejected = 0;
    std::size_t duplicates = 0;
    std::size_t bytes_read = 0;
    bool truncated_by_limits = false;  // max_records / max_total_bytes reached: remaining input was not read
    std::vector<Issue> issues;         // capped by DatasetLimits::max_issues
    std::map<std::string, std::size_t> accepted_by_licence;  // canonical licence -> count
    std::vector<std::size_t>
        accepted_lines;  // 1-based line numbers of accepted records (for downstream filtering)

    // True when at least one record was accepted, none was rejected and no limit stopped the read.
    bool clean() const noexcept { return accepted > 0 && rejected == 0 && !truncated_by_limits; }
};

class DatasetValidator {
public:
    explicit DatasetValidator(DatasetLimits limits = {}, DatasetPolicy policy = {})
        : limits_(limits), policy_(std::move(policy)) {}

    // Reads the whole stream (bounded). Never throws for bad data; bad lines become issues.
    ValidationReport validate(std::istream& in) const;
    ValidationReport validate_string(const std::string& jsonl) const;

private:
    DatasetLimits limits_;
    DatasetPolicy policy_;
};

}  // namespace hcs::train
