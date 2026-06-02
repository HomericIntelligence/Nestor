#pragma once

#include "projectnestor/reconnect_policy.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "nats.h"
#include "nlohmann/json.hpp"

// Forward-declare the opaque nats.c connection type so we can declare the
// callback shims without pulling in the entire nats.h in this header.
struct __natsConnection;

namespace projectnestor {

/// NATS JetStream client with transparent reconnection.
///
/// Two-layer reconnect strategy:
///   1. Library-level: natsOptions_SetAllowReconnect + callbacks handle
///      runtime drops after a successful initial connection.
///   2. External retry loop (reconnect_thread_): exponential backoff + jitter
///      for initial NATS_NO_SERVER and the defensive ClosedCB case.
///
/// A dedicated provisioner_thread_ recreates the jsCtx and calls
/// ensure_streams() after every (re)connect event so that JetStream work
/// never executes on a nats.c callback thread (re-entrancy safety).
class NatsClient {
 public:
  explicit NatsClient(const std::string& url);
  ~NatsClient();

  // Returns true if the initial connection succeeded.
  // On failure, a background reconnect loop starts automatically.
  bool connect();

  void close();
  [[nodiscard]] bool is_connected() const noexcept;

  // Ensure the homeric-research JetStream stream exists.
  // Called by the provisioner thread after each successful (re)connect.
  void ensure_streams();

  // Publish payload to subject via JetStream.
  // Returns false if not connected or publish fails.
  bool publish(const std::string& subject, const std::string& payload);

  // Publish a structured log event to hi.logs.nestor.* (ADR-005).
  // Fire-and-forget: never fails the caller if NATS is unavailable.
  void publish_log(const std::string& subject, const std::string& level, const std::string& message,
                   const nlohmann::json& metadata, const std::string& trace_id = "");

 private:
  // ── connection state ──────────────────────────────────────────────────────
  std::string url_;
  natsOptions* opts_ = nullptr;
  natsConnection* conn_ = nullptr;
  jsCtx* js_ = nullptr;

  // Atomic flag; written only by callbacks, close(), and try_connect_once().
  std::atomic<bool> connected_{false};

  // Monotonically increasing; bumped on each successful (re)connect so the
  // provisioner can detect new connection events.
  std::atomic<std::uint64_t> generation_{0};

  // ── synchronisation ───────────────────────────────────────────────────────
  std::mutex state_mu_;
  std::condition_variable_any wake_cv_;

  // stop_source shared between close() and provision_jetstream_locked().
  // Tokens from this source are checked inside provisioner internals.
  std::stop_source stop_src_;

  // ── provisioner-private state (accessed only under state_mu_) ─────────────
  std::uint64_t last_provisioned_gen_ = 0;
  unsigned provision_attempts_ = 0;
  // epoch value (default-constructed) == "no retry pending".
  std::chrono::steady_clock::time_point provision_retry_deadline_{};

  // ── background threads ────────────────────────────────────────────────────
  std::jthread reconnect_thread_;
  std::jthread provisioner_thread_;

  // ── backoff policy ────────────────────────────────────────────────────────
  ReconnectPolicy policy_{};  // default: base=100ms, cap=2s, jitter 0.5-1.5x

  // ── private helpers ───────────────────────────────────────────────────────

  // Attempt a single synchronous connect using opts_. Returns true on success.
  // Does NOT create a JetStream context (provisioner handles that).
  bool try_connect_once();

  // Background thread: retries try_connect_once() with backoff until
  // connected or stop requested.
  void reconnect_loop(std::stop_token st);

  // Background thread: waits for generation_ to advance (new connection),
  // then provisions JetStream and streams. Retries provisioning on failure.
  void provisioner_loop(std::stop_token st);

  // Provision JetStream context and streams. Called by provisioner_loop while
  // holding state_mu_. Returns true on full success.
  bool provision_jetstream_locked();

  // Member handlers invoked from the C callback shims below.
  void on_disconnected();
  void on_reconnected();
  void on_closed();

  // Static C-callback shims that match natsConnectionHandler typedef:
  //   typedef void (*natsConnectionHandler)(natsConnection* nc, void* closure);
  // Declared as static members so they have access to private handlers.
  static void shim_disconnected(__natsConnection* nc, void* closure);
  static void shim_reconnected(__natsConnection* nc, void* closure);
  static void shim_closed(__natsConnection* nc, void* closure);
};

}  // namespace projectnestor
