#pragma once
#include <string>

#include "nats.h"
#include "nlohmann/json.hpp"

namespace projectnestor {

class NatsClient {
 public:
  explicit NatsClient(const std::string& url);
  ~NatsClient();

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
                   const nlohmann::json& metadata, const std::string& trace_id = "");

 private:
  std::string url_;
  natsConnection* conn_ = nullptr;
  jsCtx* js_ = nullptr;
  bool connected_ = false;
};

}  // namespace projectnestor
