// Nestor TLS configuration — C++20
//
// Reads TLS settings from environment variables. Returns std::nullopt on
// configuration error (caller should exit 1).

#pragma once

#include <optional>
#include <ostream>
#include <string>

namespace nestor {

/// TLS configuration for the HTTP server.
struct TlsConfig {
  bool enabled{false};
  std::string cert_path;
  std::string key_path;

  /// Parse TLS configuration from environment variables.
  ///
  /// Variables:
  ///   NESTOR_TLS_ENABLED  — "true"/"1" enables TLS (default: false / disabled)
  ///   NESTOR_TLS_CERT     — path to PEM certificate (required when TLS enabled)
  ///   NESTOR_TLS_KEY      — path to PEM private key  (required when TLS enabled)
  ///
  /// Returns std::nullopt (and writes a diagnostic to @p err) when:
  ///   - NESTOR_TLS_ENABLED has an unrecognised value
  ///   - TLS is enabled but NESTOR_TLS_CERT or NESTOR_TLS_KEY are absent
  ///   - TLS is enabled but the cert or key files cannot be opened for reading
  ///
  /// On success always returns a TlsConfig (enabled=false is valid and means
  /// the plaintext httplib::Server path is used).
  [[nodiscard]] static std::optional<TlsConfig> from_env(std::ostream& err);
};

}  // namespace nestor
