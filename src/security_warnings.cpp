// Nestor security posture warnings — C++20

#include "nestor/security_warnings.hpp"

namespace nestor {

void log_security_posture(std::ostream& out, const std::string& bind_addr, bool tls_enabled) {
  const bool wide_bind = (bind_addr != "127.0.0.1");

  if (wide_bind && !tls_enabled) {
    out << "[SECURITY] WARNING: server bound to " << bind_addr
        << " with TLS disabled; research task data is exposed on all"
           " interfaces in cleartext. Set NESTOR_TLS_ENABLED=true and"
           " provide NESTOR_TLS_CERT/NESTOR_TLS_KEY to remediate.\n";
  } else if (wide_bind) {
    out << "[SECURITY] WARNING: server bound to " << bind_addr
        << " (all interfaces). Consider restricting to 127.0.0.1 with"
           " NESTOR_BIND_ADDR.\n";
  } else if (!tls_enabled) {
    out << "[SECURITY] WARNING: TLS is disabled; data transmitted in"
           " cleartext. Set NESTOR_TLS_ENABLED=true and provide"
           " NESTOR_TLS_CERT/NESTOR_TLS_KEY for encrypted transport.\n";
  }
}

}  // namespace nestor
