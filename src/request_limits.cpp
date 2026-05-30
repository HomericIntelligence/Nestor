// request_limits.cpp — Input validation for research endpoints.
// Issue #41/#67: enforce body-size cap and required field presence/type.

#include "projectnestor/request_limits.hpp"

namespace projectnestor {

std::optional<ValidationError> validate_research_submission(const json& body) {
  // "idea" must be present and a non-empty string.
  if (!body.contains("idea")) {
    return ValidationError{400, "Missing required field: 'idea'"};
  }
  if (!body["idea"].is_string()) {
    return ValidationError{400, "Field 'idea' must be a string"};
  }
  if (body["idea"].get<std::string>().empty()) {
    return ValidationError{400, "Field 'idea' must not be empty"};
  }

  // "context" is optional, but if present must be a string.
  if (body.contains("context") && !body["context"].is_string()) {
    return ValidationError{400, "Field 'context' must be a string"};
  }

  return std::nullopt;
}

std::optional<ValidationError> validate_completion(const json& body) {
  // "summary" must be present and a non-empty string.
  if (!body.contains("summary")) {
    return ValidationError{400, "Missing required field: 'summary'"};
  }
  if (!body["summary"].is_string()) {
    return ValidationError{400, "Field 'summary' must be a string"};
  }
  if (body["summary"].get<std::string>().empty()) {
    return ValidationError{400, "Field 'summary' must not be empty"};
  }

  return std::nullopt;
}

}  // namespace projectnestor
