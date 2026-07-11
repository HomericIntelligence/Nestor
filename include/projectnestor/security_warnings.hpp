// Nestor security posture warnings — C++20
//
// Logs startup warnings when the server's security posture is suboptimal.
// Separated from server_main.cpp so the logic can be unit-tested without
// spinning up a real server.

#pragma once

#include <ostream>
#include <string>

namespace nestor {

/// Log security posture warnings to @p out based on bind address and TLS state.
///
/// Warning matrix:
///   bind != 127.0.0.1  only  → [SECURITY] WARNING: server bound to <addr> (all interfaces)
///   TLS disabled only        → [SECURITY] WARNING: TLS is disabled; data transmitted in cleartext
///   both                     → [SECURITY] WARNING: server bound to <addr> with TLS disabled;
///                               research task data is exposed on all interfaces in cleartext
///
/// No output when bind_addr == "127.0.0.1" AND tls_enabled == true.
void log_security_posture(std::ostream& out, const std::string& bind_addr, bool tls_enabled);

}  // namespace nestor
