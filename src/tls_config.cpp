// Nestor TLS configuration implementation — C++20

#include "nestor/tls_config.hpp"

#include <cstdlib>
#include <fstream>
#include <string_view>

namespace nestor {

std::optional<TlsConfig> TlsConfig::from_env(std::ostream& err) {
  TlsConfig cfg;

  // ── Parse NESTOR_TLS_ENABLED ─────────────────────────────────────────────
  const char* tls_env = std::getenv("NESTOR_TLS_ENABLED");
  if (tls_env != nullptr) {
    const std::string_view val{tls_env};
    if (val == "true" || val == "1") {
      cfg.enabled = true;
    } else if (val == "false" || val == "0" || val.empty()) {
      cfg.enabled = false;
    } else {
      err << "[TlsConfig] NESTOR_TLS_ENABLED=\"" << tls_env
          << "\" is not recognised; expected true/1 or false/0.\n";
      return std::nullopt;
    }
  }

  if (!cfg.enabled) {
    return cfg;
  }

  // ── Require cert + key when TLS is enabled ───────────────────────────────
  const char* cert_env = std::getenv("NESTOR_TLS_CERT");
  const char* key_env = std::getenv("NESTOR_TLS_KEY");

  if (cert_env == nullptr || std::string_view{cert_env}.empty()) {
    err << "[TlsConfig] NESTOR_TLS_ENABLED=true but NESTOR_TLS_CERT is not set.\n";
    return std::nullopt;
  }
  if (key_env == nullptr || std::string_view{key_env}.empty()) {
    err << "[TlsConfig] NESTOR_TLS_ENABLED=true but NESTOR_TLS_KEY is not set.\n";
    return std::nullopt;
  }

  cfg.cert_path = cert_env;
  cfg.key_path = key_env;

  // ── Verify readability (catches permission-denied at config-load time) ────
  if (!std::ifstream(cfg.cert_path).good()) {
    err << "[TlsConfig] Cannot open cert file for reading: " << cfg.cert_path << "\n";
    return std::nullopt;
  }
  if (!std::ifstream(cfg.key_path).good()) {
    err << "[TlsConfig] Cannot open key file for reading: " << cfg.key_path << "\n";
    return std::nullopt;
  }

  return cfg;
}

}  // namespace nestor
