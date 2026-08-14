// Nestor HTTP Server — C++20

#include "nestor/auth.hpp"
#include "nestor/nats_client.hpp"
#include "nestor/rate_limiter.hpp"
#include "nestor/routes.hpp"
#include "nestor/security_warnings.hpp"
#include "nestor/store.hpp"
#include "nestor/tls_config.hpp"
#include "nestor/version.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>

#include "httplib.h"

namespace {
// g_server is a base-class pointer so the same signal handler works for both
// httplib::Server and httplib::SSLServer. stop() is declared on Server and is
// safe to call through the base pointer for both subclasses in cpp-httplib
// 0.18.3 (verified: SSLServer::listen_after_bind() uses the same svr_sock_
// member; stop() closes it via Server::stop()).
httplib::Server* g_server = nullptr;

void signal_handler(int /*signal*/) {
  // Async-signal-safe: write(2) is on the POSIX list; std::cout is NOT.
  // POSIX.1-2024 §2.4.3 — calling non-async-signal-safe functions from a
  // signal handler is undefined behaviour.
  static constexpr char kMsg[] = "\nShutting down Nestor...\n";
  // Best-effort write; ignore EINTR/short-write — we're tearing down anyway.
  (void)::write(STDERR_FILENO, kMsg, sizeof(kMsg) - 1);
  if (g_server != nullptr) {
    g_server->stop();
  }
}
}  // namespace

int main() {
  // ── Bind address (default: loopback only) ────────────────────────────────
  const std::string host = []() -> std::string {
    const char* env = std::getenv("NESTOR_BIND_ADDR");
    return (env != nullptr && env[0] != '\0') ? env : "127.0.0.1";
  }();

  // ── Port ─────────────────────────────────────────────────────────────────
  const int port = []() -> int {
    constexpr int kDefaultPort = 8081;
    const char* env = std::getenv("NESTOR_PORT");
    if (env == nullptr) {
      return kDefaultPort;
    }
    try {
      return std::stoi(env);
    } catch (const std::exception& e) {
      std::cerr << "[main] Invalid NESTOR_PORT=\"" << env << "\" (" << e.what()
                << "); falling back to " << kDefaultPort << ".\n";
      return kDefaultPort;
    }
  }();

  // ── TLS configuration ─────────────────────────────────────────────────────
  const auto tls = nestor::TlsConfig::from_env(std::cerr);
  if (!tls.has_value()) {
    return 1;
  }

  // ── Security posture warning ──────────────────────────────────────────────
  nestor::log_security_posture(std::cerr, host, tls->enabled);

  // ── NATS URL ─────────────────────────────────────────────────────────────
  const std::string nats_url = []() -> std::string {
    const char* env = std::getenv("NATS_URL");
    return env != nullptr ? env : "nats://localhost:4222";
  }();

  auto auth_cfg = nestor::load_auth_config_from_env();
  if (!auth_cfg) {
    std::cerr << "NESTOR_AUTH_TOKEN is not set (required in auth mode 'required')\n";
    return 1;
  }

  const std::size_t max_items = []() -> std::size_t {
    constexpr std::size_t kDefault = nestor::Store::kDefaultMaxItems;
    const char* env = std::getenv("NESTOR_MAX_ITEMS");
    if (env == nullptr) {
      return kDefault;
    }
    try {
      const unsigned long long v = std::stoull(env);
      return static_cast<std::size_t>(v);
    } catch (const std::exception& e) {
      std::cerr << "[main] Invalid NESTOR_MAX_ITEMS=\"" << env << "\" (" << e.what()
                << "); falling back to " << kDefault << ".\n";
      return kDefault;
    }
  }();

  const long pending_ttl_seconds = []() -> long {
    constexpr long kDefault = nestor::Store::kDefaultPendingTtlSeconds;
    const char* env = std::getenv("NESTOR_PENDING_TTL_SECONDS");
    if (env == nullptr) {
      return kDefault;
    }
    try {
      return std::stol(env);
    } catch (const std::exception& e) {
      std::cerr << "[main] Invalid NESTOR_PENDING_TTL_SECONDS=\"" << env << "\" (" << e.what()
                << "); falling back to " << kDefault << ".\n";
      return kDefault;
    }
  }();

  std::cout << nestor::kProjectName << " v" << nestor::kVersion << "\n";

  const std::string scheme = tls->enabled ? "https" : "http";
  std::cout << "Starting " << scheme << " server on " << host << ":" << port << "\n";

  // ── Rate limiter ─────────────────────────────────────────────────────────
  // Read config from environment before connecting NATS so we can log early.
  // NESTOR_RATELIMIT_DISABLE=1 is a fail-loud escape hatch: it emits an ERROR
  // log and a NATS audit event so any production misconfiguration is visible.
  // See: include/nestor/rate_limiter.hpp for the full invariant doc.
  const nestor::RateLimitConfig rl_cfg = nestor::RateLimitConfig::from_env();

  nestor::Store store(max_items, std::chrono::seconds{pending_ttl_seconds});
  nestor::NatsClient nats(nats_url);

  // Close store items when a research myrmidon publishes a terminal status on
  // hi.research.{id} (Odysseus ADR-013 §7). Must be registered before
  // connect(); runs on a nats.c delivery thread (Store is mutex-guarded).
  nats.set_research_status_handler(
      [&store](const std::string& subject, const std::string& payload) {
        nestor::handle_research_status(store, subject, payload);
      });

  // Graceful degradation: server runs even if NATS is unavailable at startup.
  // On connect() failure the client starts a background reconnect loop; JetStream
  // provisioning (including ensure_streams()) is handled by the provisioner thread.
  if (!nats.connect()) {
    std::cout << "[main] NATS unreachable at startup — retrying in background.\n";
  }

  // Construct the rate limiter on main()'s stack BEFORE the TLS/plain branch so
  // both server instantiations can capture it (it outlives server.listen()).
  if (rl_cfg.disabled) {
    // Fail-loud: emit ERROR to stderr AND publish a NATS audit event so the
    // disable flag is visible in central log aggregation (ADR-005).
    // DO NOT SET NESTOR_RATELIMIT_DISABLE=1 IN PRODUCTION.
    std::cerr << "[ratelimit] ERROR: DISABLED via NESTOR_RATELIMIT_DISABLE"
                 " — DO NOT USE IN PRODUCTION\n";
    nats.publish_log("hi.logs.nestor.ratelimit_disabled", "error",
                     "Rate limiting DISABLED via NESTOR_RATELIMIT_DISABLE"
                     " — DO NOT USE IN PRODUCTION",
                     {});
  } else {
    std::cout << "[ratelimit] INFO: enabled default_rps=" << rl_cfg.default_rps
              << " default_burst=" << rl_cfg.default_burst
              << " research_rps=" << rl_cfg.research_rps
              << " research_burst=" << rl_cfg.research_burst << "\n";
  }

  // Construct limiter on main() stack — outlives server.listen() which blocks
  // until shutdown. Pointer captured by route lambdas (never dangling).
  nestor::RateLimiter limiter{rl_cfg};

  // ── Server instantiation (TLS branch) ────────────────────────────────────
  // The server is heap-allocated so the signal trampoline's g_server pointer
  // never holds a stack address (CodeQL cpp/stack-address-escape). The heap
  // object outlives listen(); main() returns only after listen() returns.
  if (tls->enabled) {
    auto server = std::make_unique<httplib::SSLServer>(tls->cert_path.c_str(),
                                                       tls->key_path.c_str());
    if (!server->is_valid()) {
      std::cerr << "[main] TLS server init failed; check cert/key paths.\n";
      return 1;
    }
    // Upcast: g_server is httplib::Server* — stop() is safe through base ptr.
    g_server = server.get();

    server->set_payload_max_length(1 * 1024 * 1024);  // 1 MiB
    server->set_read_timeout(5, 0);
    server->set_write_timeout(5, 0);

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    nestor::install_auth_middleware(*server, *auth_cfg);
    nestor::register_routes(*server, store, nats, limiter);

    std::cout << "Routes registered. Listening...\n";
    if (!server->listen(host, port)) {
      std::cerr << "Failed to start server on port " << port << "\n";
      return 1;
    }
  } else {
    auto server = std::make_unique<httplib::Server>();
    g_server = server.get();

    server->set_payload_max_length(1 * 1024 * 1024);  // 1 MiB
    server->set_read_timeout(5, 0);
    server->set_write_timeout(5, 0);

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    nestor::install_auth_middleware(*server, *auth_cfg);
    nestor::register_routes(*server, store, nats, limiter);

    std::cout << "Routes registered. Listening...\n";
    if (!server->listen(host, port)) {
      std::cerr << "Failed to start server on port " << port << "\n";
      return 1;
    }
  }
  return 0;
}
