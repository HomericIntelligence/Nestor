// ProjectNestor HTTP Server — C++20

#include "projectnestor/auth.hpp"
#include "projectnestor/nats_client.hpp"
#include "projectnestor/routes.hpp"
#include "projectnestor/store.hpp"
#include "projectnestor/version.hpp"

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

  std::cout << projectnestor::kProjectName << " v" << projectnestor::kVersion << "\n";
  std::cout << "Starting HTTP server on " << host << ":" << port << "\n";

  projectnestor::Store store;
  projectnestor::NatsClient nats(nats_url);

  // Graceful degradation: server runs even if NATS is unavailable.
  if (!nats.connect()) {
    std::cout << "[main] NATS unavailable — running without event publishing.\n";
  } else {
    nats.ensure_streams();
  }

  httplib::Server server;
  g_server = &server;

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  projectnestor::install_auth_middleware(server, *auth_cfg);
  projectnestor::register_routes(server, store, nats);

  std::cout << "Routes registered. Listening...\n";
  if (!server.listen(host, port)) {
    std::cerr << "Failed to start server on port " << port << "\n";
    return 1;
  }

  return 0;
}
