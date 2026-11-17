#pragma once
// Strict decoder for `data:image/<type>;base64,<payload>` URIs used by OpenAI-style image_url parts.
// Remote (http/https/file) URLs are deliberately NOT supported: the runtime makes no outbound requests
// and never reads server paths named by a request (AGENTS.md 19.1/19.2).
#include <cstddef>
#include <string_view>

#include "hcs/engine.hpp"
#include "hcs/result.hpp"

namespace hcs::api {

// InvalidArgument on: non-data URI, media type other than image/{png,jpeg,jpg,gif,bmp}, missing ";base64",
// malformed base64 (alphabet, padding, whitespace is rejected), or decoded size above `max_bytes`
// (checked from the encoded length BEFORE any allocation). The decoded bytes are not otherwise inspected.
Result<ImageInput> decode_image_data_uri(std::string_view uri, std::size_t max_bytes);

}  // namespace hcs::api
