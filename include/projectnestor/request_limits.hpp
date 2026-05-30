#pragma once
// request_limits.hpp — Input validation helpers.
//
// Issue #41: POST /v1/research accepted unbounded body, enabling memory
// exhaustion DoS before even reaching json::parse.
//
// Issue #67: POST /v1/research/:id/complete accepted empty body with no
// metadata validation.

#include <optional>
#include <string>

#include "nlohmann/json.hpp"

namespace projectnestor {

using json = nlohmann::json;

// Maximum allowed request body size (64 KiB). Requests larger than this are
// rejected with HTTP 413 before JSON parsing begins.
inline constexpr std::size_t kMaxBodyBytes = 64u * 1024u;

// Validation error: holds an HTTP status code and detail message.
struct ValidationError {
  int status;
  std::string detail;
};

// Validate a research submission body (POST /v1/research).
// Rules:
//   - "idea" field must be present and must be a non-empty string.
//   - "context" field, if present, must be a string.
//   - No field may be a non-string type where a string is expected.
// Returns std::nullopt on success, or a ValidationError on failure.
[[nodiscard]] std::optional<ValidationError> validate_research_submission(const json& body);

// Validate a research completion body (POST /v1/research/:id/complete).
// Rules:
//   - "summary" field must be present and must be a non-empty string.
// Returns std::nullopt on success, or a ValidationError on failure.
[[nodiscard]] std::optional<ValidationError> validate_completion(const json& body);

}  // namespace projectnestor
