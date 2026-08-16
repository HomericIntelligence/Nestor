// Nestor TLS configuration implementation — C++20

#include "nestor/tls_config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>

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

  // ── Resolve + validate operator-supplied TLS material paths ───────────────
  // NESTOR_TLS_CERT/KEY are deployment config (like the bind address), but we
  // still harden the open: require an absolute path to an existing regular
  // file so a misconfigured environment cannot point the server at arbitrary
  // paths (CodeQL cpp/path-injection).
  const auto resolve_tls_file = [&err](const char* env_path, const char* label,
                                       std::string& out) -> bool {
    if (env_path == nullptr || env_path[0] == '\0') {
      err << "[TlsConfig] " << label << " is empty.\n";
      return false;
    }
    std::error_code ec;
    std::filesystem::path raw{env_path};
    if (!raw.is_absolute()) {
      err << "[TlsConfig] " << label << " must be an absolute path: " << env_path << "\n";
      return false;
    }
    const std::filesystem::path canon = std::filesystem::weakly_canonical(raw, ec);
    if (ec) {
      err << "[TlsConfig] " << label << " cannot be resolved: " << env_path << "\n";
      return false;
    }
    if (!std::filesystem::is_regular_file(canon, ec) || ec) {
      err << "[TlsConfig] " << label << " is not a regular file: " << canon << "\n";
      return false;
    }
    out = canon.string();
    return true;
  };

  if (!resolve_tls_file(cert_env, "NESTOR_TLS_CERT", cfg.cert_path)) {
    return std::nullopt;
  }
  if (!resolve_tls_file(key_env, "NESTOR_TLS_KEY", cfg.key_path)) {
    return std::nullopt;
  }

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
