#pragma once
// Issue #18: Replace void* C-handle members with typed RAII ownership.
// nats.h uses C typedefs (typedef struct __natsConnection natsConnection) which
// conflict with C++ forward declarations. We use a Pimpl to keep nats.h out of
// this public header entirely — callers no longer need nats.h in scope.

#include <memory>
#include <string>

#include "nlohmann/json.hpp"

namespace projectnestor {

class NatsClient {
 public:
  explicit NatsClient(const std::string& url);
  ~NatsClient();

  // Non-copyable, non-movable (owns C-library resources).
  NatsClient(const NatsClient&) = delete;
  NatsClient& operator=(const NatsClient&) = delete;
  NatsClient(NatsClient&&) = delete;
  NatsClient& operator=(NatsClient&&) = delete;

  // Returns true if connection succeeded.
  bool connect();
  void close();
  bool is_connected() const;

  // Ensure the homeric-research JetStream stream exists.
  void ensure_streams();

  // Publish payload to subject. Returns false if not connected or publish fails.
  bool publish(const std::string& subject, const std::string& payload);

  // Publish a structured log event to hi.logs.nestor.* (ADR-005).
  // Fire-and-forget: never fails the caller if NATS is unavailable.
  void publish_log(const std::string& subject, const std::string& level, const std::string& message,
                   const nlohmann::json& metadata);

 private:
  // Pimpl: keeps nats.h (and its C typedefs) out of the public header.
  // Issue #18: typed natsConnection* and jsCtx* live in the Impl struct defined
  // in nats_client.cpp, eliminating void* + reinterpret_cast at call sites.
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace projectnestor
