// ProjectNestor HTTP Server — C++20

#include "projectnestor/auth.hpp"
#include "projectnestor/nats_client.hpp"
#include "projectnestor/rate_limiter.hpp"
#include "projectnestor/routes.hpp"
#include "projectnestor/store.hpp"
#include "projectnestor/version.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <unistd.h>

#include "httplib.h"

namespace {
httplib::Server* g_server = nullptr;

void signal_handler(int /*signal*/) {
  // Async-signal-safe: write(2) is on the POSIX list; std::cout is NOT.
  // POSIX.1-2024 §2.4.3 — calling non-async-signal-safe functions from a
  // signal handler is undefined behaviour.
  static constexpr char kMsg[] = "\nShutting down ProjectNestor...\n";
  // Best-effort write; ignore EINTR/short-write — we're tearing down anyway.
  (void)::write(STDERR_FILENO, kMsg, sizeof(kMsg) - 1);
  if (g_server != nullptr) {
    g_server->stop();
  }
}
}  // namespace

int main() {
  const std::string host = "0.0.0.0";
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

  const std::string nats_url = []() -> std::string {
    const char* env = std::getenv("NATS_URL");
    return env != nullptr ? env : "nats://localhost:4222";
  }();

  auto auth_cfg = projectnestor::load_auth_config_from_env();
  if (!auth_cfg) {
    std::cerr << "NESTOR_AUTH_TOKEN is not set (required in auth mode 'required')\n";
    return 1;
  }

  const std::size_t max_items = []() -> std::size_t {
    constexpr std::size_t kDefault = projectnestor::Store::kDefaultMaxItems;
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
    constexpr long kDefault = projectnestor::Store::kDefaultPendingTtlSeconds;
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

  std::cout << projectnestor::kProjectName << " v" << projectnestor::kVersion << "\n";
  std::cout << "Starting HTTP server on " << host << ":" << port << "\n";

  // ── Rate limiter ─────────────────────────────────────────────────────────
  // Read config from environment before connecting NATS so we can log early.
  // NESTOR_RATELIMIT_DISABLE=1 is a fail-loud escape hatch: it emits an ERROR
  // log and a NATS audit event so any production misconfiguration is visible.
  // See: include/projectnestor/rate_limiter.hpp for the full invariant doc.
  const projectnestor::RateLimitConfig rl_cfg = projectnestor::RateLimitConfig::from_env();

  projectnestor::Store store(max_items, std::chrono::seconds{pending_ttl_seconds});
  projectnestor::NatsClient nats(nats_url);

  // Graceful degradation: server runs even if NATS is unavailable at startup.
  // On connect() failure the client starts a background reconnect loop; JetStream
  // provisioning (including ensure_streams()) is handled by the provisioner thread.
  if (!nats.connect()) {
    std::cout << "[main] NATS unreachable at startup — retrying in background.\n";
  }

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
  projectnestor::RateLimiter limiter{rl_cfg};

  httplib::Server server;
  g_server = &server;

  server.set_payload_max_length(1 * 1024 * 1024);  // 1 MiB
  server.set_read_timeout(5, 0);
  server.set_write_timeout(5, 0);

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  projectnestor::install_auth_middleware(server, *auth_cfg);
  projectnestor::register_routes(server, store, nats, limiter);

  std::cout << "Routes registered. Listening...\n";
  if (!server.listen(host, port)) {
    std::cerr << "Failed to start server on port " << port << "\n";
    return 1;
  }

  return 0;
}
