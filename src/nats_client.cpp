// Nestor NATS client — wraps nats.c for JetStream publishing.
// Implements two-layer reconnection: library-level callbacks (runtime drops)
// + external retry loop (initial connect failure / ClosedCB defensive path).
// A separate provisioner thread owns all JetStream context creation so that
// nats.c callback threads never perform re-entrant JetStream RPCs.

#include "nestor/nats_client.hpp"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "nats.h"
#include "nlohmann/json.hpp"

namespace nestor {

struct NatsClient::CallbackState {
  class Lease {
   public:
    explicit Lease(std::shared_ptr<CallbackState> state) : state_(std::move(state)) {
      if (state_ != nullptr) {
        active_ = state_->try_enter();
      }
    }
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&& other) noexcept
        : state_(std::move(other.state_)), active_(std::exchange(other.active_, false)) {}
    ~Lease() {
      if (active_) {
        state_->leave();
      }
    }
    explicit operator bool() const noexcept { return active_; }
    CallbackState* operator->() const noexcept { return state_.get(); }

   private:
    std::shared_ptr<CallbackState> state_;
    bool active_ = false;
  };

  static std::shared_ptr<CallbackState> find_connection(natsConnection* nc) {
    std::scoped_lock lk(registry_mu_);
    const auto it = connections_.find(nc);
    return it == connections_.end() ? nullptr : it->second.lock();
  }

  static bool register_connection(natsConnection* nc, const std::shared_ptr<CallbackState>& state) {
    std::scoped_lock event_lk(state->event_mu_);
    if (state->is_closing()) {
      return false;
    }
    {
      std::scoped_lock registry_lk(registry_mu_);
      connections_[nc] = state;  // weak_ptr: registry never owns callback state
    }
    if (state->is_closing()) {
      unregister_connection(nc);
      return false;
    }
    state->set_connection_status_locked(natsConnection_Status(nc) == NATS_CONN_STATUS_CONNECTED);
    return true;
  }

  static void unregister_connection(natsConnection* nc) {
    if (nc == nullptr) {
      return;
    }
    std::scoped_lock lk(registry_mu_);
    connections_.erase(nc);
  }

  static std::shared_ptr<CallbackState> find_subscription(natsSubscription* sub) {
    std::scoped_lock lk(registry_mu_);
    const auto it = subscriptions_.find(sub);
    return it == subscriptions_.end() ? nullptr : it->second.lock();
  }

  static void register_subscription(natsSubscription* sub,
                                    const std::shared_ptr<CallbackState>& state) {
    std::scoped_lock lk(registry_mu_);
    subscriptions_[sub] = state;
  }

  static bool has_subscription(natsSubscription* sub) {
    std::scoped_lock lk(registry_mu_);
    return subscriptions_.contains(sub);
  }

  static void unregister_subscription(natsSubscription* sub) {
    if (sub == nullptr) {
      return;
    }
    std::scoped_lock lk(registry_mu_);
    subscriptions_.erase(sub);
  }

  void set_handler(MessageHandler handler) {
    std::scoped_lock lk(lifecycle_mu_);
    handler_ = std::move(handler);
  }

  MessageHandler handler_snapshot() {
    std::scoped_lock lk(lifecycle_mu_);
    return handler_;
  }

  bool has_handler() {
    std::scoped_lock lk(lifecycle_mu_);
    return static_cast<bool>(handler_);
  }

  bool is_closing() {
    std::scoped_lock lk(lifecycle_mu_);
    return closing_;
  }

  void begin_close() {
    std::scoped_lock lk(lifecycle_mu_);
    closing_ = true;
  }

  void wait_for_active_callbacks() {
    std::unique_lock lk(lifecycle_mu_);
    callbacks_done_cv_.wait(lk, [this] { return active_callbacks_ == 0; });
  }

  bool is_connected() const noexcept { return connected_.load(std::memory_order_acquire); }
  std::uint64_t generation() const noexcept { return generation_.load(std::memory_order_acquire); }

  void mark_disconnected() {
    std::scoped_lock lk(event_mu_);
    set_connection_status_locked(false);
  }

  void mark_reconnected() {
    std::scoped_lock lk(event_mu_);
    set_connection_status_locked(true);
  }

  void mark_subscription_complete(natsSubscription* sub) {
    {
      std::scoped_lock lk(completion_mu_);
      completed_subscriptions_.insert(sub);
    }
    completion_cv_.notify_all();
  }

  bool wait_for_subscription_complete(natsSubscription* sub, std::chrono::milliseconds timeout) {
    std::unique_lock lk(completion_mu_);
    const bool completed = completion_cv_.wait_for(
        lk, timeout, [this, sub] { return completed_subscriptions_.contains(sub); });
    completed_subscriptions_.erase(sub);
    return completed;
  }

  void forget_subscription(natsSubscription* sub) {
    std::scoped_lock lk(completion_mu_);
    completed_subscriptions_.erase(sub);
  }

  std::condition_variable_any wake_cv_;

 private:
  bool try_enter() {
    std::scoped_lock lk(lifecycle_mu_);
    if (closing_) {
      return false;
    }
    ++active_callbacks_;
    return true;
  }

  void leave() {
    {
      std::scoped_lock lk(lifecycle_mu_);
      --active_callbacks_;
    }
    callbacks_done_cv_.notify_all();
  }

  void set_connection_status_locked(bool connected) {
    const bool was_connected = connected_.exchange(connected, std::memory_order_acq_rel);
    if (connected && !was_connected) {
      generation_.fetch_add(1, std::memory_order_acq_rel);
    }
    wake_cv_.notify_all();
  }

  std::mutex lifecycle_mu_;
  std::condition_variable callbacks_done_cv_;
  bool closing_ = false;
  std::size_t active_callbacks_ = 0;
  MessageHandler handler_;
  std::mutex event_mu_;
  std::atomic<bool> connected_{false};
  std::atomic<std::uint64_t> generation_{0};
  std::mutex completion_mu_;
  std::condition_variable completion_cv_;
  std::unordered_set<natsSubscription*> completed_subscriptions_;
  inline static std::mutex registry_mu_;
  inline static std::unordered_map<natsConnection*, std::weak_ptr<CallbackState>> connections_;
  inline static std::unordered_map<natsSubscription*, std::weak_ptr<CallbackState>> subscriptions_;
};

// ─── Construction / Destruction ───────────────────────────────────────────────

NatsClient::NatsClient(const std::string& url)
    : url_(url), callback_state_(std::make_shared<CallbackState>()) {}

NatsClient::~NatsClient() { close(); }

// ─── Static C-callback shims ─────────────────────────────────────────────────
// nats.c retains the connection/subscription objects while it invokes their
// callbacks. The pointers are opaque registry keys; callbacks never dereference
// NatsClient or a raw CallbackState closure.

// LCOV_EXCL_START — fired only by nats.c on a real connection drop/reconnect;
// covered by integration tests against a live broker.
void NatsClient::shim_disconnected(__natsConnection* nc, void* /*closure*/) noexcept {
  try {
    CallbackState::Lease lease(
        CallbackState::find_connection(reinterpret_cast<natsConnection*>(nc)));
    if (!lease) {
      return;
    }
    lease->mark_disconnected();
    std::fputs("[NatsClient] Disconnected from NATS.\n", stderr);
  } catch (...) {
    std::fputs("[NatsClient] disconnected callback failed.\n", stderr);
  }
}

void NatsClient::shim_reconnected(__natsConnection* nc, void* /*closure*/) noexcept {
  try {
    CallbackState::Lease lease(
        CallbackState::find_connection(reinterpret_cast<natsConnection*>(nc)));
    if (!lease) {
      return;
    }
    lease->mark_reconnected();
    std::fputs("[NatsClient] Reconnected to NATS.\n", stdout);
  } catch (...) {
    std::fputs("[NatsClient] reconnected callback failed.\n", stderr);
  }
}
// LCOV_EXCL_STOP

void NatsClient::shim_closed(__natsConnection* nc, void* /*closure*/) noexcept {
  try {
    CallbackState::Lease lease(
        CallbackState::find_connection(reinterpret_cast<natsConnection*>(nc)));
    if (!lease) {
      return;
    }
    lease->mark_disconnected();
    std::fputs("[NatsClient] NATS connection closed (ClosedCB).\n", stderr);
  } catch (...) {
    std::fputs("[NatsClient] closed callback failed.\n", stderr);
  }
}

// LCOV_EXCL_START — invoked only on a nats.c delivery thread for messages
// arriving over a live connection; covered by integration tests.
void NatsClient::shim_research_message(__natsConnection* nc, __natsSubscription* /*sub*/,
                                       __natsMsg* msg, void* /*closure*/) noexcept {
  auto* m = reinterpret_cast<natsMsg*>(msg);
  try {
    CallbackState::Lease lease(
        CallbackState::find_connection(reinterpret_cast<natsConnection*>(nc)));
    if (lease) {
      MessageHandler handler = lease->handler_snapshot();
      if (handler) {
        const char* subject = natsMsg_GetSubject(m);
        const char* data = natsMsg_GetData(m);
        const int len = natsMsg_GetDataLength(m);
        try {
          handler(std::string(subject != nullptr ? subject : ""),
                  std::string(data != nullptr ? data : "", static_cast<std::size_t>(len)));
        } catch (const std::exception& e) {
          std::fprintf(stderr, "[NatsClient] research status handler threw: %s\n", e.what());
        } catch (...) {
          std::fputs("[NatsClient] research status handler threw a non-standard exception.\n",
                     stderr);
        }
      }
    }
  } catch (...) {
    std::fputs("[NatsClient] research message callback failed.\n", stderr);
  }
  natsMsg_Destroy(m);
}
// LCOV_EXCL_STOP

void NatsClient::shim_subscription_complete(void* closure) noexcept {
  auto* sub = static_cast<natsSubscription*>(closure);
  try {
    const std::shared_ptr<CallbackState> state = CallbackState::find_subscription(sub);
    if (state != nullptr) {
      state->mark_subscription_complete(sub);
    }
  } catch (...) {
    // Completion is advisory. close() uses a bounded wait and remains safe
    // because callback admission and registry ownership are independent.
  }
}

// ─── set_research_status_handler() ───────────────────────────────────────────

void NatsClient::set_research_status_handler(MessageHandler handler) {
  callback_state_->set_handler(std::move(handler));
}

// ─── connect() ────────────────────────────────────────────────────────────────

bool NatsClient::connect() {
  std::scoped_lock lifecycle_lk(close_mu_);
  if (callback_state_->is_closing()) {
    return false;
  }

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

  natsOptions_SetDisconnectedCB(
      opts, reinterpret_cast<natsConnectionHandler>(NatsClient::shim_disconnected), nullptr);
  natsOptions_SetReconnectedCB(
      opts, reinterpret_cast<natsConnectionHandler>(NatsClient::shim_reconnected), nullptr);
  natsOptions_SetClosedCB(opts, reinterpret_cast<natsConnectionHandler>(NatsClient::shim_closed),
                          nullptr);

  // Start the provisioner before attempting the first connect so it's ready
  // to process the generation bump from a successful try_connect_once().
  provisioner_thread_ = std::jthread([this](std::stop_token st) { provisioner_loop(st); });

  // Attempt initial connection.
  const bool ok = try_connect_once();
  if (!ok && !callback_state_->is_closing()) {
    // Start background retry loop.
    reconnect_thread_ = std::jthread([this](std::stop_token st) { reconnect_loop(st); });
    std::cout << "[NatsClient] NATS unreachable at startup — retrying in background.\n";
  }
  return ok;
}

// ─── try_connect_once() ───────────────────────────────────────────────────────

bool NatsClient::try_connect_once() {
  const std::shared_ptr<CallbackState> callbacks = callback_state_;
  if (callbacks->is_closing()) {
    return false;
  }

  natsConnection* nc = nullptr;
  const natsStatus s = natsConnection_Connect(&nc, reinterpret_cast<natsOptions*>(opts_));
  if (s != NATS_OK) {
    std::cerr << "[NatsClient] Connect attempt to " << url_ << " failed: " << natsStatus_GetText(s)
              << "\n";
    return false;
  }

  natsConnection* previous = nullptr;
  bool accepted = false;
  {
    std::scoped_lock lk(connection_mu_, state_mu_);
    if (!callbacks->is_closing() && CallbackState::register_connection(nc, callbacks)) {
      previous = reinterpret_cast<natsConnection*>(conn_);
      conn_ = nc;
      accepted = true;
    }
  }

  if (!accepted) {
    natsConnection_Destroy(nc);
    return false;
  }
  if (previous != nullptr) {
    CallbackState::unregister_connection(previous);
    natsConnection_Destroy(previous);
  }

  std::cout << "[NatsClient] Connected to " << url_ << "\n";
  callbacks->wake_cv_.notify_all();
  return true;
}

// ─── reconnect_loop() ─────────────────────────────────────────────────────────

// LCOV_EXCL_START — the retry loop can only terminate successfully against a
// live NATS broker; the wake-on-stop path is timing-dependent. Covered by
// integration tests.
void NatsClient::reconnect_loop(std::stop_token st) {
  const std::shared_ptr<CallbackState> callbacks = callback_state_;
  std::mt19937 rng{std::random_device{}()};
  unsigned attempt = 0;

  while (!st.stop_requested() && !callbacks->is_connected()) {
    const auto delay = next_delay(policy_, attempt++, rng);

    {
      std::unique_lock<std::mutex> lk(state_mu_);
      // Interruptible sleep: wakes early on stop or successful connect.
      callbacks->wake_cv_.wait_for(
          lk, st, delay, [&] { return st.stop_requested() || callbacks->is_connected(); });
    }

    if (st.stop_requested() || callbacks->is_connected()) {
      return;
    }

    if (try_connect_once()) {
      return;  // Provisioner will handle JetStream from here.
    }
  }
}
// LCOV_EXCL_STOP

// ─── provisioner_loop() ───────────────────────────────────────────────────────

// LCOV_EXCL_START — provisioning work only happens after a successful connect
// bumps generation_; unit tests only exercise the wait/stop path. Covered by
// integration tests.
void NatsClient::provisioner_loop(std::stop_token st) {
  const std::shared_ptr<CallbackState> callbacks = callback_state_;
  std::mt19937 rng{std::random_device{}()};

  while (!st.stop_requested()) {
    std::unique_lock<std::mutex> lk(state_mu_);

    // Determine whether a provisioner retry deadline is active.
    const bool retry_pending = provision_retry_deadline_ != std::chrono::steady_clock::time_point{};

    if (retry_pending) {
      // Wait until the deadline or a relevant state change.
      callbacks->wake_cv_.wait_until(lk, st, provision_retry_deadline_, [&] {
        return st.stop_requested() || callbacks->generation() > last_provisioned_gen_;
      });
    } else {
      // Wait indefinitely for a new connection event.
      callbacks->wake_cv_.wait(lk, st, [&] {
        return st.stop_requested() ||
               (callbacks->is_connected() && callbacks->generation() > last_provisioned_gen_);
      });
    }

    if (st.stop_requested()) {
      return;
    }

    if (!callbacks->is_connected()) {
      // Connection dropped; clear provisioner retry state and wait again.
      provision_attempts_ = 0;
      provision_retry_deadline_ = {};
      continue;
    }

    if (callbacks->generation() <= last_provisioned_gen_ && !retry_pending) {
      continue;
    }

    // Provision JetStream while holding state_mu_.
    if (provision_jetstream_locked()) {
      last_provisioned_gen_ = callbacks->generation();
      provision_attempts_ = 0;
      provision_retry_deadline_ = {};
    } else if (callbacks->is_connected()) {
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
// LCOV_EXCL_STOP

// ─── provision_jetstream_locked() ────────────────────────────────────────────
// Caller holds state_mu_.

bool NatsClient::provision_jetstream_locked() {
  // Destroy any existing JetStream context.
  if (js_ != nullptr) {
    jsCtx_Destroy(js_);
    js_ = nullptr;
  }

  if (stop_src_.stop_requested() || callback_state_->is_closing() || conn_ == nullptr) {
    return false;
  }

  // LCOV_EXCL_START — JetStream RPCs require a live broker.
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

  if (stop_src_.stop_requested() || callback_state_->is_closing()) {
    jsCtx_Destroy(new_js);
    return false;
  }

  js_ = new_js;

  // Provision streams (idempotent). Failure must reach provisioner_loop so
  // its backoff retry engages — but the core research-status subscription is
  // independent of stream provisioning, so (re)create it regardless.
  const bool streams_ok = ensure_streams();

  if (stop_src_.stop_requested() || callback_state_->is_closing()) {
    return false;
  }

  // (Re)create the core research-status subscription (ADR-013 §7).
  resubscribe_research_locked();

  return streams_ok;
  // LCOV_EXCL_STOP
}

// ─── close() ─────────────────────────────────────────────────────────────────

void NatsClient::close() {
  std::scoped_lock lifecycle_lk(close_mu_);
  const std::shared_ptr<CallbackState> callbacks = callback_state_;

  // Gate admission first. A callback that already acquired a lease is counted
  // and close waits for it below; callbacks that arrive later cannot call user code.
  callbacks->begin_close();
  stop_src_.request_stop();
  reconnect_thread_.request_stop();
  provisioner_thread_.request_stop();
  callbacks->wake_cv_.notify_all();

  // Snapshot with the dedicated connection lock, then close without holding a
  // Nestor mutex. This unblocks any JetStream RPC while the provisioner owns
  // state_mu_.
  natsConnection* connection = nullptr;
  {
    std::scoped_lock connection_lk(connection_mu_);
    connection = reinterpret_cast<natsConnection*>(conn_);
  }
  if (connection != nullptr) {
    natsConnection_Close(connection);
  }

  // No connection handle or options can be destroyed until the only thread
  // that calls natsConnection_Connect() and the provisioner have stopped.
  if (reconnect_thread_.joinable()) {
    reconnect_thread_.join();
  }
  if (provisioner_thread_.joinable()) {
    provisioner_thread_.join();
  }

  natsSubscription* subscription = nullptr;
  jsCtx* js = nullptr;
  natsOptions* options = nullptr;
  {
    std::scoped_lock lk(connection_mu_, state_mu_);
    subscription = reinterpret_cast<natsSubscription*>(research_sub_);
    research_sub_ = nullptr;
    js = reinterpret_cast<jsCtx*>(js_);
    js_ = nullptr;
    connection = reinterpret_cast<natsConnection*>(conn_);
    conn_ = nullptr;
    options = reinterpret_cast<natsOptions*>(opts_);
    opts_ = nullptr;
  }

  // This wait is deliberate: a user handler admitted before the closing gate
  // can still own caller resources. close() does not return until that known
  // active handler returns. It does not wait indefinitely for a library event.
  callbacks->wait_for_active_callbacks();

  // nats.c documents OnComplete as the point after the final async message
  // handler. The notification is advisory and can be dropped on allocation or
  // shutdown failure, so its wait is bounded. Registry erasure is the fallback.
  if (subscription != nullptr && CallbackState::has_subscription(subscription)) {
    (void)callbacks->wait_for_subscription_complete(subscription, std::chrono::milliseconds{500});
  }

  CallbackState::unregister_subscription(subscription);
  callbacks->forget_subscription(subscription);
  CallbackState::unregister_connection(connection);

  if (subscription != nullptr) {
    natsSubscription_Destroy(subscription);
  }
  if (js != nullptr) {
    jsCtx_Destroy(js);
  }
  if (connection != nullptr) {
    natsConnection_Destroy(connection);
  }
  if (options != nullptr) {
    natsOptions_Destroy(options);
  }

  callbacks->mark_disconnected();
}

// ─── is_connected() ───────────────────────────────────────────────────────────

bool NatsClient::is_connected() const noexcept { return callback_state_->is_connected(); }

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

  // LCOV_EXCL_START — js_AddStream RPCs require a live broker; the js_ guard
  // above is exercised by EnsureStreamsWhenNotConnectedReturnsEarly.
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
  // LCOV_EXCL_STOP
}

// ─── resubscribe_research_locked() ───────────────────────────────────────────
// Caller holds state_mu_. Destroys any prior subscription and creates a fresh
// core (non-JetStream) subscription on hi.research.> for status updates.

// LCOV_EXCL_START — only reachable from provision_jetstream_locked() after a
// successful connect; the Subscribe RPC needs a live broker. Covered by
// integration tests.
void NatsClient::resubscribe_research_locked() {
  const std::shared_ptr<CallbackState> callbacks = callback_state_;
  if (!callbacks->has_handler() || callbacks->is_closing() || conn_ == nullptr) {
    return;
  }
  if (research_sub_ != nullptr) {
    auto* old_sub = reinterpret_cast<natsSubscription*>(research_sub_);
    CallbackState::unregister_subscription(old_sub);
    callbacks->forget_subscription(old_sub);
    natsSubscription_Destroy(old_sub);
    research_sub_ = nullptr;
  }
  natsSubscription* sub = nullptr;
  const natsStatus s = natsConnection_Subscribe(
      &sub, reinterpret_cast<natsConnection*>(conn_), "hi.research.>",
      reinterpret_cast<natsMsgHandler>(NatsClient::shim_research_message), nullptr);
  if (s != NATS_OK) {
    std::cerr << "[NatsClient] Subscribe to hi.research.> failed: " << natsStatus_GetText(s)
              << "\n";
    return;
  }

  CallbackState::register_subscription(sub, callbacks);
  const natsStatus completion_status = natsSubscription_SetOnCompleteCB(
      sub, NatsClient::shim_subscription_complete, static_cast<void*>(sub));
  if (completion_status != NATS_OK) {
    CallbackState::unregister_subscription(sub);
    std::cerr << "[NatsClient] Subscription completion callback unavailable: "
              << natsStatus_GetText(completion_status) << "\n";
  }

  research_sub_ = sub;
  std::cout << "[NatsClient] Subscribed to hi.research.> (status updates).\n";
}
// LCOV_EXCL_STOP

// ─── publish() ───────────────────────────────────────────────────────────────

bool NatsClient::publish(const std::string& subject, const std::string& payload) {
  // R1-#2: hold full-call lock; eliminates snapshot-under-lock complexity.
  std::scoped_lock lk(state_mu_);

  if (!callback_state_->is_connected() || js_ == nullptr) {
    return false;
  }

  // LCOV_EXCL_START — js_Publish requires a live JetStream context; the
  // disconnected guard above is exercised by PublishWhenNotConnectedReturnsFalse.
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
  // LCOV_EXCL_STOP
}

// ─── publish_log() ───────────────────────────────────────────────────────────

void NatsClient::publish_log(const std::string& subject, const std::string& level,
                             const std::string& message, const nlohmann::json& metadata,
                             const std::string& trace_id) {
  // R1-#2: hold full-call lock for consistency.
  std::scoped_lock lk(state_mu_);

  if (!callback_state_->is_connected() || conn_ == nullptr) {
    return;  // Graceful degradation — NATS unavailable.
  }

  // LCOV_EXCL_START — the fire-and-forget publish requires a live connection;
  // the disconnected early-return above is exercised by the PublishLog* tests.
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
  // LCOV_EXCL_STOP
}

}  // namespace nestor
