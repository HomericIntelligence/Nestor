#pragma once

#include <optional>
#include <string>

#include "httplib.h"

namespace nestor {

enum class AuthMode { Required, None };

struct AuthConfig {
  AuthMode mode;
  std::string token;
};

std::optional<AuthConfig> load_auth_config_from_env();

void install_auth_middleware(httplib::Server& server, const AuthConfig& cfg);

}  // namespace nestor
