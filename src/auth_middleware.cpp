// auth_middleware.cpp — Bearer-token authentication middleware implementation.
// Issue #40/#65: adds constant-time bearer-token check to all non-health routes.

#include "projectnestor/auth_middleware.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

#include "nlohmann/json.hpp"

namespace projectnestor {

namespace {

// Constant-time string comparison to prevent timing-oracle attacks.
// Returns true if lhs == rhs. Both strings are always fully traversed.
bool constant_time_equal(std::string_view lhs, std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    // Still do a dummy loop over lhs to keep timing uniform across callers.
    volatile unsigned char dummy{0};
    for (char c : lhs) {
      dummy |= static_cast<unsigned char>(c);
    }
    return false;
  }
  volatile unsigned char diff = 0;
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    diff |= static_cast<unsigned char>(lhs[i]) ^ static_cast<unsigned char>(rhs[i]);
  }
  return diff == 0;
}

// Extract bearer token from "Authorization: Bearer <token>" header.
// Returns empty string if header is absent or malformed.
std::string extract_bearer(const httplib::Request& req) {
  const std::string header = req.get_header_value("Authorization");
  constexpr std::string_view kPrefix = "Bearer ";
  if (header.size() <= kPrefix.size()) {
    return {};
  }
  if (header.substr(0, kPrefix.size()) != kPrefix) {
    return {};
  }
  return header.substr(kPrefix.size());
}

}  // namespace

AuthMiddleware::AuthMiddleware() {
  const char* env = std::getenv("NESTOR_AUTH_TOKEN");
  if (env != nullptr && env[0] != '\0') {
    token_ = env;
  } else {
    std::cerr << "[AuthMiddleware] WARNING: NESTOR_AUTH_TOKEN is not set. "
                 "All requests are accepted. Set this variable in production.\n";
  }
}

AuthMiddleware::AuthMiddleware(std::string token) : token_(std::move(token)) {}

bool AuthMiddleware::is_enabled() const noexcept { return !token_.empty(); }

bool AuthMiddleware::check_request(const httplib::Request& req, httplib::Response& res) const {
  if (!is_enabled()) {
    return true;  // Auth disabled — dev mode.
  }
  const std::string bearer = extract_bearer(req);
  if (!constant_time_equal(bearer, token_)) {
    res.status = 401;
    res.set_content(
        nlohmann::json{{"detail", "Unauthorized: valid Authorization: Bearer <token> required"}}
            .dump(),
        "application/json");
    return false;
  }
  return true;
}

}  // namespace projectnestor
