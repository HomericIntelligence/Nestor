// ProjectNestor NATS client — wraps nats.c for JetStream publishing.
// Issue #18: replaced void* + reinterpret_cast with a Pimpl holding typed
// natsConnection* and jsCtx* RAII members. nats.h is now included only here,
// keeping C typedefs out of the public header.
// Gracefully degrades: server continues without NATS if connection fails.

#include "projectnestor/nats_client.hpp"

#include <chrono>
#include <iostream>
#include <memory>

#include "nats.h"
#include "nlohmann/json.hpp"

namespace projectnestor {

// ── Pimpl definition ─────────────────────────────────────────────────────────
// Typed RAII wrappers: unique_ptr with custom deleters for nats.c handles.
// This eliminates every reinterpret_cast that existed in the previous void*
// implementation.

namespace {

struct NatsConnectionDeleter {
  void operator()(natsConnection* c) const noexcept {
    if (c != nullptr) natsConnection_Destroy(c);
  }
};

struct JsCtxDeleter {
  void operator()(jsCtx* j) const noexcept {
    if (j != nullptr) jsCtx_Destroy(j);
  }
};

}  // namespace

struct NatsClient::Impl {
  std::unique_ptr<natsConnection, NatsConnectionDeleter> conn;
  std::unique_ptr<jsCtx, JsCtxDeleter> js;
  bool connected{false};
  std::string url;
};

// ── NatsClient implementation ────────────────────────────────────────────────

NatsClient::NatsClient(const std::string& url) : impl_(std::make_unique<Impl>()) {
  impl_->url = url;
}

NatsClient::~NatsClient() { close(); }

bool NatsClient::connect() {
  natsConnection* raw_conn = nullptr;
  natsStatus s = natsConnection_ConnectTo(&raw_conn, impl_->url.c_str());
  if (s != NATS_OK) {
    std::cerr << "[NatsClient] Failed to connect to " << impl_->url << ": " << natsStatus_GetText(s)
              << "\n";
    impl_->connected = false;
    return false;
  }
  impl_->conn.reset(raw_conn);

  jsOptions js_opts;
  jsOptions_Init(&js_opts);
  jsCtx* raw_js = nullptr;
  s = natsConnection_JetStream(&raw_js, impl_->conn.get(), &js_opts);
  if (s != NATS_OK) {
    std::cerr << "[NatsClient] Failed to get JetStream context: " << natsStatus_GetText(s) << "\n";
    impl_->conn.reset();
    impl_->connected = false;
    return false;
  }
  impl_->js.reset(raw_js);

  impl_->connected = true;
  std::cout << "[NatsClient] Connected to " << impl_->url << "\n";
  return true;
}

void NatsClient::close() {
  if (!impl_) return;
  // Destroy JetStream context before the connection (correct teardown order).
  impl_->js.reset();
  impl_->conn.reset();
  impl_->connected = false;
}

bool NatsClient::is_connected() const { return impl_ && impl_->connected; }

void NatsClient::ensure_streams() {
  if (!impl_ || !impl_->connected || impl_->js == nullptr) {
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
  const natsStatus s = js_AddStream(&si, impl_->js.get(), &cfg, nullptr, &jerr);

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
  if (!impl_ || !impl_->connected || impl_->js == nullptr) {
    return false;
  }

  jsPubAck* ack = nullptr;
  jsErrCode jerr = static_cast<jsErrCode>(0);
  const natsStatus s = js_Publish(&ack, impl_->js.get(), subject.c_str(), payload.data(),
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
                             const std::string& message, const nlohmann::json& metadata) {
  if (!impl_ || !impl_->connected || impl_->conn == nullptr) {
    return;  // Graceful degradation — NATS unavailable.
  }

  const double timestamp =
      std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();

  const nlohmann::json log_payload = {
      {"timestamp", timestamp}, {"service", "nestor"},  {"level", level},
      {"message", message},     {"metadata", metadata},
  };

  const std::string payload_str = log_payload.dump();

  // Use core NATS publish (non-JetStream) for fire-and-forget log delivery.
  // Return value intentionally ignored — log publish failures are non-fatal.
  // The explicit (void) cast satisfies static analysers (cert-err33-c,
  // bugprone-unused-return-value) and documents the intent at the call site.
  (void)natsConnection_PublishString(impl_->conn.get(), subject.c_str(), payload_str.c_str());
}

}  // namespace projectnestor
