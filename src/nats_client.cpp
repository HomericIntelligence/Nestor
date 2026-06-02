// ProjectNestor NATS client — wraps nats.c for JetStream publishing.
// Gracefully degrades: server continues without NATS if connection fails.

#include "projectnestor/nats_client.hpp"

#include <chrono>
#include <iostream>

#include "nats.h"
#include "nlohmann/json.hpp"

namespace projectnestor {

NatsClient::NatsClient(const std::string& url) : url_(url) {}

NatsClient::~NatsClient() { close(); }

bool NatsClient::connect() {
  natsStatus s = natsConnection_ConnectTo(&conn_, url_.c_str());
  if (s != NATS_OK) {
    std::cerr << "[NatsClient] Failed to connect to " << url_ << ": " << natsStatus_GetText(s)
              << "\n";
    conn_ = nullptr;
    connected_ = false;
    return false;
  }

  jsOptions js_opts;
  jsOptions_Init(&js_opts);
  s = natsConnection_JetStream(&js_, conn_, &js_opts);
  if (s != NATS_OK) {
    std::cerr << "[NatsClient] Failed to get JetStream context: " << natsStatus_GetText(s) << "\n";
    natsConnection_Destroy(conn_);
    conn_ = nullptr;
    connected_ = false;
    return false;
  }

  connected_ = true;
  std::cout << "[NatsClient] Connected to " << url_ << "\n";
  return true;
}

void NatsClient::close() {
  if (js_ != nullptr) {
    jsCtx_Destroy(js_);
    js_ = nullptr;
  }
  if (conn_ != nullptr) {
    natsConnection_Destroy(conn_);
    conn_ = nullptr;
  }
  connected_ = false;
}

bool NatsClient::is_connected() const { return connected_; }

void NatsClient::ensure_streams() {
  if (!connected_ || js_ == nullptr) {
    return;
  }

  jsStreamConfig cfg;
  jsStreamConfig_Init(&cfg);
  cfg.Name = "homeric-research";
  const char* subjects[] = {"hi.research.>"};
  cfg.Subjects = subjects;
  cfg.SubjectsLen = 1;
  cfg.Retention = js_WorkQueuePolicy;
  cfg.Storage = js_FileStorage;

  jsStreamInfo* si = nullptr;
  jsErrCode jerr = static_cast<jsErrCode>(0);
  const natsStatus s = js_AddStream(&si, js_, &cfg, nullptr, &jerr);

  if (s == NATS_OK) {
    jsStreamInfo_Destroy(si);
    std::cout << "[NatsClient] Stream 'homeric-research' created.\n";
  } else if (s == NATS_ERR) {
    // Stream may already exist; treat as non-fatal.
    std::cout << "[NatsClient] Stream 'homeric-research' already exists (or minor error).\n";
  } else {
    std::cerr << "[NatsClient] Failed to create stream 'homeric-research': "
              << natsStatus_GetText(s) << " (jerr=" << jerr << ")\n";
  }
}

bool NatsClient::publish(const std::string& subject, const std::string& payload) {
  if (!connected_ || js_ == nullptr) {
    return false;
  }

  jsPubAck* ack = nullptr;
  jsErrCode jerr = static_cast<jsErrCode>(0);
  const natsStatus s = js_Publish(&ack, js_, subject.c_str(), payload.data(),
                                  static_cast<int>(payload.size()), nullptr, &jerr);

  if (ack != nullptr) {
    jsPubAck_Destroy(ack);
  }

  if (s != NATS_OK) {
    std::cerr << "[NatsClient] Publish failed on " << subject << ": " << natsStatus_GetText(s)
              << "\n";
    return false;
  }
  return true;
}

void NatsClient::publish_log(const std::string& subject, const std::string& level,
                             const std::string& message, const nlohmann::json& metadata,
                             const std::string& trace_id) {
  if (!connected_) {
    return;  // Graceful degradation — NATS unavailable.
  }

  const double timestamp =
      std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();

  const nlohmann::json payload = {
      {"timestamp", timestamp}, {"service", "nestor"},  {"level", level},
      {"message", message},     {"metadata", metadata}, {"trace_id", trace_id},
  };

  const std::string payload_str = payload.dump();

  // Use core NATS publish (non-JetStream) for fire-and-forget log delivery.
  // Return value intentionally ignored — log publish failures are non-fatal.
  // The explicit (void) cast satisfies static analysers (cert-err33-c,
  // bugprone-unused-return-value) and documents the intent at the call site.
  (void)natsConnection_PublishString(conn_, subject.c_str(), payload_str.c_str());
}

}  // namespace projectnestor
