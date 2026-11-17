#pragma once
// Minimal JSON-Schema subset validator (no external dependency). Used for HTTP request
// bodies and MCP tool arguments, so the schemas advertised to clients are the ones enforced.
//
// Supported keywords: type (string or array), properties, required, additionalProperties
// (bool), enum, const, minimum, maximum, exclusiveMinimum, exclusiveMaximum, minLength,
// maxLength, minItems, maxItems, items (single schema), pattern is NOT supported (rejected
// at validation time so a schema can never silently weaken enforcement), oneOf, anyOf.
// Unknown/annotation keywords (description, title, default, $schema, examples) are ignored.
#include <nlohmann/json.hpp>

#include "hcs/result.hpp"

namespace hcs::api {

// Returns InvalidArgument with a JSON-pointer-like path in the message on the first violation.
Result<void> validate_json(const nlohmann::json& schema, const nlohmann::json& value);

}  // namespace hcs::api
