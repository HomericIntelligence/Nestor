// Nestor NATS client — wraps nats.c for JetStream publishing.
// Implements two-layer reconnection: library-level callbacks (runtime drops)
// + external retry loop (initial connect failure / ClosedCB defensive path).
// A separate provisioner thread owns all JetStream context creation so that
// nats.c callback threads never perform re-entrant JetStream RPCs.

#include "nestor/nats_client.hpp"

#include <chrono>
#include <iostream>
#include <random>

#include "nats.h"
#include "nlohmann/json.hpp"

namespace nestor {

// ─── Construction / Destruction ───────────────────────────────────────────────

NatsClient::NatsClient(const std::string& url) : url_(url) {}

NatsClient::~NatsClient() { close(); }

// ─── Static C-callback shims ─────────────────────────────────────────────────
// nats.c expects: void(*)(natsConnection*, void* closure).
// __natsConnection (forward-declared in the header) == natsConnection typedef,
// so these shims match natsConnectionHandler exactly.

void NatsClient::shim_disconnected(__natsConnection* /*nc*/, void* closure) {
  static_cast<NatsClient*>(closure)->on_disconnected();
}
void NatsClient::shim_reconnected(__natsConnection* /*nc*/, void* closure) {
  static_cast<NatsClient*>(closure)->on_reconnected();
}
void NatsClient::shim_closed(__natsConnection* /*nc*/, void* closure) {
  static_cast<NatsClient*>(closure)->on_closed();
}

void NatsClient::shim_research_message(__natsConnection* /*nc*/, __natsSubscription* /*sub*/,
                                       __natsMsg* msg, void* closure) {
  auto* self = static_cast<NatsClient*>(closure);
  auto* m = reinterpret_cast<natsMsg*>(msg);
  // research_handler_ is immutable after connect() — safe to read unlocked
  // on this nats.c delivery thread. Never perform JetStream RPCs here.
  if (self->research_handler_) {
    const char* subject = natsMsg_GetSubject(m);
    const char* data = natsMsg_GetData(m);
    const int len = natsMsg_GetDataLength(m);
    try {
      self->research_handler_(std::string(subject != nullptr ? subject : ""),
                              std::string(data != nullptr ? data : "", static_cast<size_t>(len)));
    } catch (const std::exception& e) {
      std::cerr << "[NatsClient] research status handler threw: " << e.what() << "\n";
    }
  }
  natsMsg_Destroy(m);
}

// ─── set_research_status_handler() ───────────────────────────────────────────

void NatsClient::set_research_status_handler(MessageHandler handler) {
  research_handler_ = std::move(handler);
}

// ─── Callback handlers ────────────────────────────────────────────────────────

void NatsClient::on_disconnected() {
  connected_.store(false, std::memory_order_release);
  std::cerr << "[NatsClient] Disconnected from NATS.\n";
  wake_cv_.notify_all();
}

void NatsClient::on_reconnected() {
  connected_.store(true, std::memory_order_release);
  generation_.fetch_add(1, std::memory_order_acq_rel);
  std::cout << "[NatsClient] Reconnected to NATS.\n";
  wake_cv_.notify_all();
}

void NatsClient::on_closed() {
  connected_.store(false, std::memory_order_release);
  std::cerr << "[NatsClient] NATS connection closed (ClosedCB).\n";
  wake_cv_.notify_all();
}

// ─── connect() ────────────────────────────────────────────────────────────────

bool NatsClient::connect() {
  // Build options.
  natsOptions* opts = nullptr;
  natsStatus s = natsOptions_Create(&opts);
  if (s != NATS_OK) {
    std::cerr << "[NatsClient] natsOptions_Create failed: " << natsStatus_GetText(s) << "\n";
    return false;
  }
  opts_ = opts;

  natsOptions_SetURL(opts, url_.c_str());
  natsOptions_SetTimeout(opts, 500);                // 500 ms TCP connect timeout (R1-#3)
  natsOptions_SetAllowReconnect(opts, true);        // library-level reconnect
  natsOptions_SetMaxReconnect(opts, -1);            // infinite attempts
  natsOptions_SetReconnectWait(opts, 2000);         // 2 s between attempts
  natsOptions_SetReconnectJitter(opts, 500, 1000);  // jitter: 0.5 s plain / 1 s TLS

  // Register callbacks — static member shims forward to member handlers via closure.
  // shim_* are static methods matching natsConnectionHandler typedef.
  natsOptions_SetDisconnectedCB(
      opts, reinterpret_cast<natsConnectionHandler>(NatsClient::shim_disconnected), this);
  natsOptions_SetReconnectedCB(
      opts, reinterpret_cast<natsConnectionHandler>(NatsClient::shim_reconnected), this);
  natsOptions_SetClosedCB(opts, reinterpret_cast<natsConnectionHandler>(NatsClient::shim_closed),
                          this);

  // Start the provisioner before attempting the first connect so it's ready
  // to process the generation bump from a successful try_connect_once().
  provisioner_thread_ = std::jthread([this](std::stop_token st) { provisioner_loop(st); });

  // Attempt initial connection.
  const bool ok = try_connect_once();
  if (!ok) {
    // Start background retry loop.
    reconnect_thread_ = std::jthread([this](std::stop_token st) { reconnect_loop(st); });
    std::cout << "[NatsClient] NATS unreachable at startup — retrying in background.\n";
  }
  return ok;
}

// ─── try_connect_once() ───────────────────────────────────────────────────────

bool NatsClient::try_connect_once() {
  natsConnection* nc = nullptr;
  natsStatus s = natsConnection_Connect(&nc, reinterpret_cast<natsOptions*>(opts_));
  if (s != NATS_OK) {
    std::cerr << "[NatsClient] Connect attempt to " << url_ << " failed: " << natsStatus_GetText(s)
              << "\n";
    return false;
  }

  {
    std::scoped_lock lk(state_mu_);
    // Close any previous connection (defensive — normally nullptr here).
    if (conn_ != nullptr) {
      natsConnection_Destroy(conn_);
    }
    conn_ = nc;
    connected_.store(true, std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_acq_rel);
  }

  std::cout << "[NatsClient] Connected to " << url_ << "\n";
  wake_cv_.notify_all();
  return true;
}

// ─── reconnect_loop() ─────────────────────────────────────────────────────────

void NatsClient::reconnect_loop(std::stop_token st) {
  std::mt19937 rng{std::random_device{}()};
  unsigned attempt = 0;

  while (!st.stop_requested() && !connected_.load(std::memory_order_acquire)) {
    const auto delay = next_delay(policy_, attempt++, rng);

    {
      std::unique_lock<std::mutex> lk(state_mu_);
      // Interruptible sleep: wakes early on stop or successful connect.
      wake_cv_.wait_for(lk, st, delay, [&] {
        return st.stop_requested() || connected_.load(std::memory_order_acquire);
      });
    }

    if (st.stop_requested() || connected_.load(std::memory_order_acquire)) {
      return;
    }

    if (try_connect_once()) {
      return;  // Provisioner will handle JetStream from here.
    }
  }
}

// ─── provisioner_loop() ───────────────────────────────────────────────────────

void NatsClient::provisioner_loop(std::stop_token st) {
  std::mt19937 rng{std::random_device{}()};

  while (!st.stop_requested()) {
    std::unique_lock<std::mutex> lk(state_mu_);

    // Determine whether a provisioner retry deadline is active.
    const bool retry_pending = provision_retry_deadline_ != std::chrono::steady_clock::time_point{};

    if (retry_pending) {
      // Wait until the deadline or a relevant state change.
      wake_cv_.wait_until(lk, st, provision_retry_deadline_, [&] {
        return st.stop_requested() ||
               generation_.load(std::memory_order_acquire) > last_provisioned_gen_;
      });
    } else {
      // Wait indefinitely for a new connection event.
      wake_cv_.wait(lk, st, [&] {
        return st.stop_requested() ||
               (connected_.load(std::memory_order_acquire) &&
                generation_.load(std::memory_order_acquire) > last_provisioned_gen_);
      });
    }

    if (st.stop_requested()) {
      return;
    }

    if (!connected_.load(std::memory_order_acquire)) {
      // Connection dropped; clear provisioner retry state and wait again.
      provision_attempts_ = 0;
      provision_retry_deadline_ = {};
      continue;
    }

    if (generation_.load(std::memory_order_acquire) <= last_provisioned_gen_ && !retry_pending) {
      continue;
    }

    // Provision JetStream while holding state_mu_.
    if (provision_jetstream_locked()) {
      last_provisioned_gen_ = generation_.load(std::memory_order_acquire);
      provision_attempts_ = 0;
      provision_retry_deadline_ = {};
    } else if (connected_.load(std::memory_order_acquire)) {
      // Provisioning failed but still connected — schedule a retry.
      provision_retry_deadline_ =
          std::chrono::steady_clock::now() + next_delay(policy_, provision_attempts_++, rng);
      std::cerr << "[NatsClient] JetStream provisioning failed; retrying in "
                << next_delay(policy_, provision_attempts_ - 1, rng).count() << " ms.\n";
    } else {
      // Disconnected mid-provision — clear and wait for reconnect.
      provision_attempts_ = 0;
      provision_retry_deadline_ = {};
    }
  }
}

// ─── provision_jetstream_locked() ────────────────────────────────────────────
// Caller holds state_mu_.

bool NatsClient::provision_jetstream_locked() {
  // Destroy any existing JetStream context.
  if (js_ != nullptr) {
    jsCtx_Destroy(js_);
    js_ = nullptr;
  }

  if (stop_src_.stop_requested() || conn_ == nullptr) {
    return false;
  }

  // Create new JetStream context.
  jsOptions js_opts;
  jsOptions_Init(&js_opts);
  jsCtx* new_js = nullptr;
  natsStatus s =
      natsConnection_JetStream(&new_js, reinterpret_cast<natsConnection*>(conn_), &js_opts);
  if (s != NATS_OK) {
    std::cerr << "[NatsClient] JetStream context creation failed: " << natsStatus_GetText(s)
              << "\n";
    return false;
  }

  if (stop_src_.stop_requested()) {
    jsCtx_Destroy(new_js);
    return false;
  }

  js_ = new_js;

  // Provision streams (idempotent). Failure must reach provisioner_loop so
  // its backoff retry engages — but the core research-status subscription is
  // independent of stream provisioning, so (re)create it regardless.
  const bool streams_ok = ensure_streams();

  // (Re)create the core research-status subscription (ADR-013 §7).
  resubscribe_research_locked();

  return streams_ok;
}

// ─── close() ─────────────────────────────────────────────────────────────────

void NatsClient::close() {
  // Step 1: tear down the live connection to unblock any in-flight JetStream
  //         RPCs on the provisioner thread before we request stops.
  natsConnection* snap_conn = nullptr;
  {
    std::scoped_lock lk(state_mu_);
    snap_conn = reinterpret_cast<natsConnection*>(conn_);
  }
  if (snap_conn != nullptr) {
    natsConnection_Close(snap_conn);  // unblocks pending RPCs (R1-#4)
  }

  // Step 2: signal threads to stop and wake them.
  stop_src_.request_stop();
  reconnect_thread_.request_stop();
  provisioner_thread_.request_stop();
  wake_cv_.notify_all();

  // Step 3: join threads (jthread destructors do this, but be explicit before
  //         we destroy shared state they reference).
  if (reconnect_thread_.joinable()) {
    reconnect_thread_.join();
  }
  if (provisioner_thread_.joinable()) {
    provisioner_thread_.join();
  }

  // Step 4: destroy resources in correct order.
  {
    std::scoped_lock lk(state_mu_);
    if (research_sub_ != nullptr) {
      natsSubscription_Destroy(reinterpret_cast<natsSubscription*>(research_sub_));
      research_sub_ = nullptr;
    }
    if (js_ != nullptr) {
      jsCtx_Destroy(reinterpret_cast<jsCtx*>(js_));
      js_ = nullptr;
    }
    if (conn_ != nullptr) {
      natsConnection_Destroy(reinterpret_cast<natsConnection*>(conn_));
      conn_ = nullptr;
    }
    if (opts_ != nullptr) {
      natsOptions_Destroy(reinterpret_cast<natsOptions*>(opts_));
      opts_ = nullptr;
    }
    connected_.store(false, std::memory_order_release);
  }
}

// ─── is_connected() ───────────────────────────────────────────────────────────

bool NatsClient::is_connected() const noexcept {
  return connected_.load(std::memory_order_acquire);
}

// ─── ensure_streams() ────────────────────────────────────────────────────────
// Idempotent — safe to call after every (re)connect.

bool NatsClient::ensure_streams() {
  // Must be called with state_mu_ held OR from provisioner_loop before js_ is
  // shared. In practice, provisioner_loop calls this while holding state_mu_.
  if (js_ == nullptr) {
    // "Ensure" did not ensure — never let a null context mark a generation
    // provisioned.
    return false;
  }

  struct StreamDef {
    const char* name;
    const char* subject;
    jsRetentionPolicy retention;
  };
  // homeric-myrmidon backs the role-addressed dispatch queues (ADR-013 §1);
  // Agamemnon ensures the same stream — creation is idempotent and an
  // already-exists answer (even with a different config) is non-fatal.
  static const StreamDef kStreams[] = {
      {"homeric-research", "hi.research.>", js_WorkQueuePolicy},
      {"homeric-myrmidon", "hi.myrmidon.>", js_LimitsPolicy},
  };

  bool ok = true;
  for (const auto& sd : kStreams) {
    jsStreamConfig cfg;
    jsStreamConfig_Init(&cfg);
    cfg.Name = sd.name;
    const char* subjects[] = {sd.subject};
    cfg.Subjects = subjects;
    cfg.SubjectsLen = 1;
    cfg.Retention = sd.retention;
    cfg.Storage = js_FileStorage;

    jsStreamInfo* si = nullptr;
    jsErrCode jerr = static_cast<jsErrCode>(0);
    const natsStatus s = js_AddStream(&si, js_, &cfg, nullptr, &jerr);

    if (s == NATS_OK) {
      jsStreamInfo_Destroy(si);
      std::cout << "[NatsClient] Stream '" << sd.name << "' created.\n";
    } else if (s == NATS_ERR && jerr == JSStreamNameExistErr) {
      // Stream already exists (JetStream API error 10058) — non-fatal.
      std::cout << "[NatsClient] Stream '" << sd.name << "' exists.\n";
    } else {
      // Any other error (subject overlap 10065, account limits, invalid
      // config, timeouts) is a real provisioning failure — the provisioner
      // must retry.
      std::cerr << "[NatsClient] Failed to create stream '" << sd.name
                << "': " << natsStatus_GetText(s) << " (jerr=" << jerr << ")\n";
      ok = false;
    }
  }
  return ok;
}

// ─── resubscribe_research_locked() ───────────────────────────────────────────
// Caller holds state_mu_. Destroys any prior subscription and creates a fresh
// core (non-JetStream) subscription on hi.research.> for status updates.

void NatsClient::resubscribe_research_locked() {
  if (!research_handler_ || conn_ == nullptr) {
    return;
  }
  if (research_sub_ != nullptr) {
    natsSubscription_Destroy(reinterpret_cast<natsSubscription*>(research_sub_));
    research_sub_ = nullptr;
  }
  natsSubscription* sub = nullptr;
  const natsStatus s = natsConnection_Subscribe(
      &sub, reinterpret_cast<natsConnection*>(conn_), "hi.research.>",
      reinterpret_cast<natsMsgHandler>(NatsClient::shim_research_message), this);
  if (s != NATS_OK) {
    std::cerr << "[NatsClient] Subscribe to hi.research.> failed: " << natsStatus_GetText(s)
              << "\n";
    return;
  }
  research_sub_ = sub;
  std::cout << "[NatsClient] Subscribed to hi.research.> (status updates).\n";
}

// ─── publish() ───────────────────────────────────────────────────────────────

bool NatsClient::publish(const std::string& subject, const std::string& payload) {
  // R1-#2: hold full-call lock; eliminates snapshot-under-lock complexity.
  std::scoped_lock lk(state_mu_);

  if (!connected_.load(std::memory_order_acquire) || js_ == nullptr) {
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

// ─── publish_log() ───────────────────────────────────────────────────────────

void NatsClient::publish_log(const std::string& subject, const std::string& level,
                             const std::string& message, const nlohmann::json& metadata,
                             const std::string& trace_id) {
  // R1-#2: hold full-call lock for consistency.
  std::scoped_lock lk(state_mu_);

  if (!connected_.load(std::memory_order_acquire) || conn_ == nullptr) {
    return;  // Graceful degradation — NATS unavailable.
  }

  const double timestamp =
      std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();

  const nlohmann::json payload_json = {
      {"timestamp", timestamp}, {"service", "nestor"},  {"level", level},
      {"message", message},     {"metadata", metadata}, {"trace_id", trace_id},
  };

  const std::string payload_str = payload_json.dump();

  // Use core NATS publish (non-JetStream) for fire-and-forget log delivery.
  // Return value intentionally ignored — log publish failures are non-fatal.
  // The explicit (void) cast satisfies static analysers (cert-err33-c,
  // bugprone-unused-return-value) and documents the intent at the call site.
  (void)natsConnection_PublishString(conn_, subject.c_str(), payload_str.c_str());
}

}  // namespace nestor
