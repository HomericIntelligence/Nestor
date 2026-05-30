#pragma once
// auth_middleware.hpp — Bearer-token authentication middleware.
//
// Issue #40/#65: The API was completely unauthenticated, accepting any request
// on all endpoints. This middleware enforces a Bearer-token check on all routes
// except /v1/health.
//
// Configuration: set NESTOR_AUTH_TOKEN environment variable to your token.
// If the variable is unset or empty, all requests are accepted (dev mode).
//
// Exempt paths: /v1/health (liveness probe must be unauthenticated).
//
// Constant-time comparison is used to prevent timing-oracle attacks.

#include <optional>
#include <string>

#include "httplib.h"

namespace projectnestor {

class AuthMiddleware {
 public:
  // Construct from environment variable NESTOR_AUTH_TOKEN.
  // If the variable is absent or empty, auth is disabled (logged as a warning).
  AuthMiddleware();

  // Construct from an explicit token (primarily for tests).
  explicit AuthMiddleware(std::string token);

  // Returns true if authentication is enabled (token non-empty).
  [[nodiscard]] bool is_enabled() const noexcept;

  // Check an incoming request against the configured token.
  // Sets res.status = 401 and body if auth fails; returns false.
  // Returns true if auth passes or auth is disabled.
  // Routes should call this at the top of each handler (except /v1/health).
  [[nodiscard]] bool check_request(const httplib::Request& req, httplib::Response& res) const;

 private:
  std::string token_;
};

}  // namespace projectnestor
