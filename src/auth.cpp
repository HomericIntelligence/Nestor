// ProjectNestor authentication implementation — C++20

#include "projectnestor/auth.hpp"

#include <cstdlib>
#include <cstring>
#include <openssl/crypto.h>

namespace projectnestor {

std::optional<AuthConfig> load_auth_config_from_env() {
  // Parse NESTOR_AUTH_MODE (case-sensitive, defaults to "required")
  const char* mode_env = std::getenv("NESTOR_AUTH_MODE");
  AuthMode mode = AuthMode::Required;  // Default

  if (mode_env != nullptr) {
    const std::string mode_str(mode_env);
    if (mode_str == "required") {
      mode = AuthMode::Required;
    } else if (mode_str == "none") {
      mode = AuthMode::None;
    } else {
      // Unknown mode string (including case mismatches like "Required", "REQUIRED")
      return std::nullopt;
    }
  }

  // Parse NESTOR_AUTH_TOKEN
  const char* token_env = std::getenv("NESTOR_AUTH_TOKEN");
  std::string token;

  if (token_env != nullptr) {
    token = token_env;
  }

  // Empty string is treated as unset
  if (token.empty() && mode == AuthMode::Required) {
    return std::nullopt;
  }

  return AuthConfig{mode, token};
}

void install_auth_middleware(httplib::Server& server, const AuthConfig& cfg) {
  server.set_pre_routing_handler([cfg](const httplib::Request& req,
                                       httplib::Response& res) -> httplib::Server::HandlerResponse {
    // If auth mode is None, all requests are allowed.
    if (cfg.mode == AuthMode::None) {
      return httplib::Server::HandlerResponse::Unhandled;
    }

    // Auth mode is Required. Check the Authorization header.
    const std::string auth_header = req.get_header_value("Authorization");

    // Expected format: "Bearer <token>"
    const std::string bearer_prefix = "Bearer ";
    if (auth_header.empty() || auth_header.substr(0, bearer_prefix.size()) != bearer_prefix) {
      res.status = 401;
      res.set_content(R"({"detail":"unauthorized"})", "application/json");
      return httplib::Server::HandlerResponse::Handled;
    }

    // Extract token from header
    const std::string header_token = auth_header.substr(bearer_prefix.size());

    // Constant-time comparison
    // First check lengths (structural info, not secret)
    if (header_token.size() != cfg.token.size()) {
      res.status = 401;
      res.set_content(R"({"detail":"unauthorized"})", "application/json");
      return httplib::Server::HandlerResponse::Handled;
    }

    // Compare using CRYPTO_memcmp to prevent timing attacks
    if (CRYPTO_memcmp(header_token.data(), cfg.token.data(), cfg.token.size()) != 0) {
      res.status = 401;
      res.set_content(R"({"detail":"unauthorized"})", "application/json");
      return httplib::Server::HandlerResponse::Handled;
    }

    // Token matches, allow the request to reach the route handler
    return httplib::Server::HandlerResponse::Unhandled;
  });
}

}  // namespace projectnestor
